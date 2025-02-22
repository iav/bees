#include "bees.h"

#include "crucible/btrfs-tree.h"
#include "crucible/cache.h"
#include "crucible/ntoa.h"
#include "crucible/string.h"
#include "crucible/task.h"

#include <algorithm>
#include <fstream>
#include <tuple>

using namespace crucible;
using namespace std;

string
format_time(time_t t)
{
	const struct tm *const tmp = localtime(&t);
	char buf[1024];
	strftime(buf, sizeof(buf), "%Y-%m-%d-%H-%M-%S", tmp);
	return buf;
}

ostream &
operator<<(ostream &os, const BeesCrawlState &bcs)
{
	const time_t now = time(NULL);
	const auto age = now - bcs.m_started;
	return os << "BeesCrawlState "
		<< bcs.m_root << ":" << bcs.m_objectid << " offset " << to_hex(bcs.m_offset)
		<< " transid " << bcs.m_min_transid << ".." << bcs.m_max_transid
		<< " started " << format_time(bcs.m_started) << " (" << age << "s ago)";
}

BeesCrawlState::BeesCrawlState() :
	m_root(0),
	m_objectid(0),
	m_offset(0),
	m_min_transid(0),
	m_max_transid(0),
	m_started(time(NULL))
{
}

bool
BeesCrawlState::operator<(const BeesCrawlState &that) const
{
	return tie(m_min_transid, m_max_transid, m_objectid, m_offset, m_root)
		< tie(that.m_min_transid, that.m_max_transid, that.m_objectid, that.m_offset, that.m_root);
}

class BeesScanMode {
protected:
	shared_ptr<BeesRoots>	m_roots;
	bool crawl_batch(const shared_ptr<BeesCrawl>& crawl);
public:
	virtual ~BeesScanMode() {}
	BeesScanMode(const shared_ptr<BeesRoots>& roots) : m_roots(roots) {}
	virtual bool scan() = 0;
	using CrawlMap = decltype(BeesRoots::m_root_crawl_map);
	virtual void next_transid(const CrawlMap &crawl_map) = 0;
	virtual const char *ntoa() const = 0;
};

bool
BeesScanMode::crawl_batch(const shared_ptr<BeesCrawl>& crawl)
{
	return m_roots->crawl_batch(crawl);
}

/// Scan the same inode/offset tuple in each subvol.  Good for caching and space saving,
/// bad for filesystems with rotating snapshots.
class BeesScanModeLockstep : public BeesScanMode {
	using SortKey = tuple<uint64_t, uint64_t, uint64_t>;
	using Map = map<SortKey, CrawlMap::mapped_type>;
	mutex m_mutex;
	shared_ptr<Map> m_sorted;
public:
	using BeesScanMode::BeesScanMode;
	~BeesScanModeLockstep() override {}
	bool scan() override;
	void next_transid(const CrawlMap &crawl_map) override;
	const char *ntoa() const override;
};

const char *
BeesScanModeLockstep::ntoa() const
{
	return "LOCKSTEP";
}

bool
BeesScanModeLockstep::scan()
{
	unique_lock<mutex> lock(m_mutex);
	const auto hold_sorted = m_sorted;
	lock.unlock();
	if (!hold_sorted) {
		BEESLOGINFO("called Lockstep scan without a sorted map");
		return false;
	}
	auto &sorted = *hold_sorted;
	while (!sorted.empty()) {
		const auto this_crawl = sorted.begin()->second;
		sorted.erase(sorted.begin());
		const bool rv = crawl_batch(this_crawl);
		if (rv) {
			const auto this_range = this_crawl->peek_front();
			if (this_range) {
				const auto new_key = SortKey(this_range.fid().ino(), this_range.begin(), this_range.fid().root());
				const auto new_value = make_pair(new_key, this_crawl);
				const auto insert_rv = sorted.insert(new_value);
				THROW_CHECK0(runtime_error, insert_rv.second);
			}
			return true;
		}
	}
	return false;
}

void
BeesScanModeLockstep::next_transid(const CrawlMap &crawl_map)
{
	auto new_map = make_shared<Map>();
	for (const auto &i : crawl_map) {
		const auto this_crawl = i.second;
		const auto this_range = this_crawl->peek_front();
		if (this_range) {
			const auto new_key = SortKey(this_range.fid().ino(), this_range.begin(), this_range.fid().root());
			const auto new_value = make_pair(new_key, this_crawl);
			const auto insert_rv = new_map->insert(new_value);
			THROW_CHECK0(runtime_error, insert_rv.second);
		}
	}
	unique_lock<mutex> lock(m_mutex);
	swap(m_sorted, new_map);
}

/// Scan each subvol in round-robin with no synchronization.
/// Good for continuous forward progress while avoiding lock contention.
class BeesScanModeIndependent : public BeesScanMode {
	using List = list<CrawlMap::mapped_type>;
	mutex m_mutex;
	shared_ptr<List> m_subvols;
public:
	using BeesScanMode::BeesScanMode;
	~BeesScanModeIndependent() override {}
	bool scan() override;
	void next_transid(const CrawlMap &crawl_map) override;
	const char *ntoa() const override;
};

const char *
BeesScanModeIndependent::ntoa() const
{
	return "INDEPENDENT";
}

bool
BeesScanModeIndependent::scan()
{
	unique_lock<mutex> lock(m_mutex);
	const auto hold_subvols = m_subvols;
	lock.unlock();
	if (!hold_subvols) {
		BEESLOGINFO("called Independent scan without a subvol list");
		return false;
	}
	auto &subvols = *hold_subvols;
	while (!subvols.empty()) {
		const auto this_crawl = *subvols.begin();
		subvols.erase(subvols.begin());
		const bool rv = crawl_batch(this_crawl);
		if (rv) {
			subvols.push_back(this_crawl);
			return true;
		}
	}
	return false;
}

void
BeesScanModeIndependent::next_transid(const CrawlMap &crawl_map)
{
	auto new_subvols = make_shared<List>();
	for (const auto &i : crawl_map) {
		const auto this_crawl = i.second;
		const auto this_range = this_crawl->peek_front();
		if (this_range) {
			new_subvols->push_back(this_crawl);
		}
	}
	unique_lock<mutex> lock(m_mutex);
	swap(m_subvols, new_subvols);
}

/// Scan each subvol completely, in numerical order, before moving on to the next.
/// This was an experimental mode that requires large amounts of temporary space
/// and has the lowest hit rate.
class BeesScanModeSequential : public BeesScanMode {
	using SortKey = uint64_t;
	using Map = map<SortKey, CrawlMap::mapped_type>;
	mutex m_mutex;
	shared_ptr<Map> m_sorted;
public:
	using BeesScanMode::BeesScanMode;
	~BeesScanModeSequential() override {}
	bool scan() override;
	void next_transid(const CrawlMap &crawl_map) override;
	const char *ntoa() const override;
};

const char *
BeesScanModeSequential::ntoa() const
{
	return "SEQUENTIAL";
}

bool
BeesScanModeSequential::scan()
{
	unique_lock<mutex> lock(m_mutex);
	const auto hold_sorted = m_sorted;
	lock.unlock();
	if (!hold_sorted) {
		BEESLOGINFO("called Sequential scan without a sorted map");
		return false;
	}
	auto &sorted = *hold_sorted;
	while (!sorted.empty()) {
		const auto this_crawl = sorted.begin()->second;
		const bool rv = crawl_batch(this_crawl);
		if (rv) {
			return true;
		} else {
			sorted.erase(sorted.begin());
		}
	}
	return false;
}

void
BeesScanModeSequential::next_transid(const CrawlMap &crawl_map)
{
	auto new_map = make_shared<Map>();
	for (const auto &i : crawl_map) {
		const auto this_crawl = i.second;
		const auto this_range = this_crawl->peek_front();
		if (this_range) {
			const auto new_key = this_range.fid().root();
			const auto new_value = make_pair(new_key, this_crawl);
			const auto insert_rv = new_map->insert(new_value);
			THROW_CHECK0(runtime_error, insert_rv.second);
		}
	}
	unique_lock<mutex> lock(m_mutex);
	swap(m_sorted, new_map);
}

/// Scan the most recently completely scanned subvols first.  Keeps recently added data
/// from accumulating in small subvols while large subvols are still undergoing their first scan.
class BeesScanModeRecent : public BeesScanMode {
	struct SortKey {
		uint64_t min_transid, max_transid;
		bool operator<(const SortKey &that) const {
			return tie(that.min_transid, that.max_transid) < tie(min_transid, max_transid);
		}
	};
	using Map = map<SortKey, list<CrawlMap::mapped_type>>;
	mutex m_mutex;
	shared_ptr<Map> m_sorted;
public:
	using BeesScanMode::BeesScanMode;
	~BeesScanModeRecent() override {}
	bool scan() override;
	void next_transid(const CrawlMap &crawl_map) override;
	const char *ntoa() const override;
};

const char *
BeesScanModeRecent::ntoa() const
{
	return "RECENT";
}

bool
BeesScanModeRecent::scan()
{
	unique_lock<mutex> lock(m_mutex);
	const auto hold_sorted = m_sorted;
	lock.unlock();
	if (!hold_sorted) {
		BEESLOGINFO("called Recent scan without a sorted map");
		return false;
	}
	auto &sorted = *hold_sorted;
	while (!sorted.empty()) {
		auto &this_list = sorted.begin()->second;
		if (this_list.empty()) {
			sorted.erase(sorted.begin());
		} else {
			const auto this_crawl = *this_list.begin();
			this_list.erase(this_list.begin());
			const bool rv = crawl_batch(this_crawl);
			if (rv) {
				this_list.push_back(this_crawl);
				return true;
			}
		}
	}
	return false;
}

void
BeesScanModeRecent::next_transid(const CrawlMap &crawl_map)
{
	auto new_map = make_shared<Map>();
	auto &sorted = *new_map;
	for (const auto &i : crawl_map) {
		const auto this_crawl = i.second;
		const auto this_range = this_crawl->peek_front();
		if (this_range) {
			const auto state_end = this_crawl->get_state_end();
			const auto min_transid = state_end.m_min_transid;
			// Should we use max_transid or only min_transid?
			// Using max_transid here would make it more like sequential,
			// and sequential is bad.
			// const auto max_transid = state_end.m_max_transid;
			const auto max_transid = 0;
			const SortKey key { .min_transid = min_transid, .max_transid = max_transid };
			sorted[key].push_back(this_crawl);
		}
	}
	unique_lock<mutex> lock(m_mutex);
	swap(m_sorted, new_map);
}

void
BeesRoots::set_scan_mode(ScanMode mode)
{
	THROW_CHECK1(invalid_argument, mode, mode < SCAN_MODE_COUNT);
	unique_lock<mutex> lock(m_mutex);
	switch (mode) {
		case SCAN_MODE_LOCKSTEP: {
			m_scanner = make_shared<BeesScanModeLockstep>(shared_from_this());
			break;
		}
		case SCAN_MODE_INDEPENDENT: {
			m_scanner = make_shared<BeesScanModeIndependent>(shared_from_this());
			break;
		}
		case SCAN_MODE_SEQUENTIAL: {
			m_scanner = make_shared<BeesScanModeSequential>(shared_from_this());
			break;
		}
		case SCAN_MODE_RECENT: {
			m_scanner = make_shared<BeesScanModeRecent>(shared_from_this());
			break;
		}
		case SCAN_MODE_COUNT:
		default:
			assert(false);
			break;
	}
	BEESLOGINFO("Scan mode set to " << mode << " (" << m_scanner->ntoa() << ")");
}

void
BeesRoots::set_workaround_btrfs_send(bool do_avoid)
{
	m_workaround_btrfs_send = do_avoid;
	if (m_workaround_btrfs_send) {
		BEESLOGINFO("WORKAROUND: btrfs send workaround enabled");
	} else {
		BEESLOGINFO("btrfs send workaround disabled");
	}
}

string
BeesRoots::crawl_state_filename() const
{
	// Legacy filename included UUID.  That feature was removed in 2016.
	return "beescrawl.dat";
}

ostream &
BeesRoots::state_to_stream(ostream &ofs)
{
	for (auto i : m_root_crawl_map) {
		auto ibcs = i.second->get_state_begin();
		if (ibcs.m_max_transid) {
			ofs << "root "        << ibcs.m_root                 << " ";
			ofs << "objectid "    << ibcs.m_objectid             << " ";
			ofs << "offset "      << ibcs.m_offset               << " ";
			ofs << "min_transid " << ibcs.m_min_transid          << " ";
			ofs << "max_transid " << ibcs.m_max_transid          << " ";
			ofs << "started "     << ibcs.m_started              << " ";
			ofs << "start_ts "    << format_time(ibcs.m_started) << "\n";
		}
	}
	return ofs;
}

void
BeesRoots::state_save()
{
	BEESNOTE("saving crawl state");
	BEESLOGINFO("Saving crawl state");
	BEESTOOLONG("Saving crawl state");

	Timer save_time;

	unique_lock<mutex> lock(m_mutex);

	// We don't have ofstreamat or ofdstream in C++11, so we're building a string and writing it with raw syscalls.
	ostringstream ofs;

	if (m_crawl_clean == m_crawl_dirty) {
		BEESLOGINFO("Nothing to save");
		return;
	}

	state_to_stream(ofs);
	const auto crawl_saved = m_crawl_dirty;

	if (ofs.str().empty()) {
		BEESLOGWARN("Crawl state empty!");
		m_crawl_clean = crawl_saved;
		return;
	}

	lock.unlock();

	// This may throw an exception, so we didn't save the state we thought we did.
	m_crawl_state_file.write(ofs.str());

	BEESNOTE("relocking crawl state to update dirty/clean state");
	lock.lock();
	// This records the version of the crawl state we saved, which is not necessarily the current state
	m_crawl_clean = crawl_saved;
	BEESLOGINFO("Saved crawl state in " << save_time << "s");
}

void
BeesRoots::crawl_state_set_dirty()
{
	unique_lock<mutex> lock(m_mutex);
	++m_crawl_dirty;
}

void
BeesRoots::crawl_state_erase(const BeesCrawlState &bcs)
{
	unique_lock<mutex> lock(m_mutex);

	// Do not delete the last entry, it holds our max_transid
	if (m_root_crawl_map.size() < 2) {
		BEESCOUNT(crawl_no_empty);
		return;
	}

	if (m_root_crawl_map.count(bcs.m_root)) {
		m_root_crawl_map.erase(bcs.m_root);
		++m_crawl_dirty;
	}
}

uint64_t
BeesRoots::transid_min()
{
	uint64_t rv = numeric_limits<uint64_t>::max();
	uint64_t last_root = 0;
	BEESNOTE("Calculating transid_min (" << rv << " so far, last_root " << last_root << ")");
	unique_lock<mutex> lock(m_mutex);
	if (m_root_crawl_map.empty()) {
		return 0;
	}
	const uint64_t max_rv = rv;
	for (auto i : m_root_crawl_map) {
		// Do not count subvols that are isolated by btrfs send workaround.
		// They will not advance until the workaround is removed or they are set read-write.
		catch_all([&](){
			if (!is_root_ro(i.first)) {
				rv = min(rv, i.second->get_state_end().m_min_transid);
			}
		});
		last_root = i.first;
	}
	// If we get through this loop without setting rv, we'll create broken crawlers due to integer overflow.
	THROW_CHECK2(runtime_error, rv, max_rv, max_rv > rv);
	return rv;
}

uint64_t
BeesRoots::transid_max_nocache()
{
	BEESNOTE("Calculating transid_max");
	BEESTRACE("Calculating transid_max");

	// We look for the root of the extent tree and read its transid.
	// Should run in O(1) time and be fairly reliable.
	const auto bti = m_root_fetcher.root(BTRFS_EXTENT_TREE_OBJECTID);
	BEESTRACE("extracting transid from " << bti);
	const auto rv = bti.transid();

	// transid must be greater than zero, or we did something very wrong
	THROW_CHECK1(runtime_error, rv, rv > 0);
	// transid must be less than max, or we did something very wrong
	THROW_CHECK1(runtime_error, rv, rv < numeric_limits<uint64_t>::max());
	return rv;
}

uint64_t
BeesRoots::transid_max()
{
	return m_transid_re.count();
}

struct BeesFileCrawl {
	shared_ptr<BeesContext>				m_ctx;
	shared_ptr<BeesCrawl>				m_crawl;
	shared_ptr<BeesRoots>				m_roots;
	/// Progress tracker hold object
	ProgressTracker<BeesCrawlState>::ProgressHolder m_hold;
	/// Crawl state snapshot when created
	BeesCrawlState					m_state;
	/// Currently processed offset in file
	off_t						m_offset;
	/// Btrfs file fetcher
	BtrfsExtentDataFetcher				m_bedf;

	/// Method that does one unit of work for the Task
	bool crawl_one_extent();
};

bool
BeesFileCrawl::crawl_one_extent()
{
	BEESNOTE("crawl_one_extent m_offset " << to_hex(m_offset) << " state " << m_state);
	BEESTRACE("crawl_one_extent m_offset " << to_hex(m_offset) << " state " << m_state);

	// Only one thread can dedupe a file.  btrfs will lock others out.
	// Inodes are usually full of shared extents, especially in the case of snapshots,
	// so when we lock an inode, we'll lock the same inode number in all subvols at once.
	auto inode_mutex = m_ctx->get_inode_mutex(m_bedf.objectid());
	auto inode_lock = inode_mutex->try_lock(Task::current_task());
	if (!inode_lock) {
		BEESCOUNT(scanf_deferred_inode);
		// Returning false here means we won't reschedule ourselves, but inode_mutex will do that
		return false;
	}

	// If we hit an exception here we don't try to catch it.
	// It will mean the file or subvol was deleted or there's metadata corruption,
	// and we should stop trying to scan the inode in that case.
	// The calling Task will be aborted.
	const auto bti = m_bedf.lower_bound(m_offset);
	if (!bti) {
		return false;
	}
	// Make sure we advance
	m_offset = max(bti.offset() + m_bedf.block_size(), bti.offset());
	// Check extent item generation is in range
	const auto gen = bti.file_extent_generation();
	if (gen < m_state.m_min_transid) {
		BEESCOUNT(crawl_gen_low);
		// The header generation refers to the transid
		// of the metadata page holding the current ref.
		// This includes anything else in that page that
		// happened to be modified, regardless of how
		// old it is.
		// The file_extent_generation refers to the
		// transid of the extent item's page, which is
		// what we really want when we are slicing up
		// the extent data by transid.
		return true;
	}
	if (gen > m_state.m_max_transid) {
		BEESCOUNT(crawl_gen_high);
		// We want to see old extents with references in
		// new pages, which means we have to get extent
		// refs from every page older than min_transid,
		// not every page between min_transid and
		// max_transid.  This means that we will get
		// refs to new extent data that we don't want to
		// process yet, because we'll process it again
		// on the next crawl cycle.  We filter out refs
		// to new extents here.
		return true;
	}

	const auto type = bti.file_extent_type();
	switch (type) {
		default:
			BEESLOGDEBUG("Unhandled file extent type " << btrfs_search_type_ntoa(type) << " in root " << m_state.m_root << " " << bti);
			BEESCOUNT(crawl_unknown);
			break;
		case BTRFS_FILE_EXTENT_INLINE:
			// Ignore these for now.
			// TODO:  replace with out-of-line dup extents
			BEESCOUNT(crawl_inline);
			break;
		case BTRFS_FILE_EXTENT_PREALLOC:
			BEESCOUNT(crawl_prealloc);
			// fallthrough
		case BTRFS_FILE_EXTENT_REG: {
			const auto physical = bti.file_extent_bytenr();
			const auto len = bti.file_extent_logical_bytes();
			BEESTRACE("Root " << m_state.m_root << " ino " << bti.objectid() << " physical " << to_hex(physical)
				<< " logical " << to_hex(bti.offset()) << ".." << to_hex(bti.offset() + len)
				<< " gen " << gen);
			if (physical) {
				THROW_CHECK1(runtime_error, len, len > 0);
				BeesFileId bfi(m_state.m_root, bti.objectid());
				if (m_ctx->is_blacklisted(bfi)) {
					BEESCOUNT(crawl_blacklisted);
				} else {
					BeesFileRange bfr(bfi, bti.offset(), bti.offset() + len);
					BEESCOUNT(crawl_push);
					auto bcs = m_state;
					bcs.m_objectid = bfr.fid().ino();
					bcs.m_offset = bfr.begin();
					const auto new_holder = m_crawl->hold_state(bcs);
					// If we hit an exception here, ignore it.
					// It might be corrupted data, the file might have been deleted or truncated,
					// or we might hit some other recoverable error.  We'll try again with
					// the next extent.
					bool scan_again = false;
					catch_all([&]() {
						BEESNOTE("scan_forward " << bfr);
						// BEESLOGDEBUG("scan_forward #" << Task::current_task().id() << " " << bfr);
						scan_again = m_ctx->scan_forward(bfr);
						// BEESLOGDEBUG("done_forward #" << Task::current_task().id() << " " << bfr);
					} );
					if (!scan_again) {
						m_hold = new_holder;
					} else {
						BEESCOUNT(crawl_again);
					}
				}
			} else {
				BEESCOUNT(crawl_hole);
			}
			break;
		}
	}
	return true;
}

bool
BeesRoots::crawl_batch(shared_ptr<BeesCrawl> this_crawl)
{
	const auto this_state = this_crawl->get_state_end();
	BEESNOTE("Crawling batch " << this_state);
	BEESTRACE("Crawling batch " << this_state);
	const auto this_range = this_crawl->pop_front();
	if (!this_range) {
		return false;
	}
	const auto subvol = this_range.fid().root();
	const auto inode = this_range.fid().ino();
	ostringstream oss;
	oss << "crawl_" << subvol << "_" << inode;
	const auto task_title = oss.str();
	const auto bfc = make_shared<BeesFileCrawl>((BeesFileCrawl) {
		.m_ctx = m_ctx,
		.m_crawl = this_crawl,
		.m_roots = shared_from_this(),
		.m_hold = this_crawl->hold_state(this_state),
		.m_state = this_state,
		.m_offset = this_range.begin(),
		.m_bedf = BtrfsExtentDataFetcher(m_ctx->root_fd()),
	});
	bfc->m_bedf.tree(subvol);
	bfc->m_bedf.objectid(inode);
	bfc->m_bedf.transid(this_state.m_min_transid);
	BEESNOTE("Starting task " << this_range);
	Task(task_title, [bfc]() {
		BEESNOTE("crawl_batch " << bfc->m_hold->get());
		if (bfc->crawl_one_extent()) {
			// Append the current task to itself to make
			// sure we keep a worker processing this file
			Task::current_task().append(Task::current_task());
		}
	}).run();
	auto next_state = this_state;
	// Skip to EOF.  Will repeat up to 16 times if there happens to be an extent at 16EB,
	// which would be a neat trick given that off64_t is signed.
	next_state.m_offset = max(next_state.m_offset, numeric_limits<uint64_t>::max() - 65536 + 1);
	this_crawl->set_state(next_state);
	BEESCOUNT(crawl_scan);
	return true;
}

bool
BeesRoots::crawl_roots()
{
	BEESNOTE("Crawling roots");
	BEESTRACE("Crawling roots");

	unique_lock<mutex> lock(m_mutex);
	const auto hold_scanner = m_scanner;
	lock.unlock();

	THROW_CHECK0(runtime_error, hold_scanner);

	BEESNOTE("Scanning roots in " << hold_scanner->ntoa() << " mode");
	BEESTRACE("scanning roots in " << hold_scanner->ntoa() << " mode");

	if (hold_scanner->scan()) {
		return true;
	}

	BEESCOUNT(crawl_done);

	const auto ran_out_time = m_crawl_timer.lap();
	BEESLOGINFO("crawl_more ran out of data after " << ran_out_time << "s");

	// Do not run again
	return false;
}

void
BeesRoots::clear_caches()
{
	m_ctx->fd_cache()->clear();
	m_ctx->resolve_cache_clear();
}

void
BeesRoots::crawl_thread()
{
	BEESNOTE("creating crawl task");

	// Create the Task that does the crawling
	const auto shared_this = shared_from_this();
	const auto crawl_task = Task("crawl_more", [shared_this]() {
		BEESTRACE("crawl_more " << shared_this);
		if (shared_this->crawl_roots()) {
			Task::current_task().run();
		}
	});
	const auto crawl_new = Task("crawl_new", [shared_this, crawl_task]() {
		BEESTRACE("crawl_new " << shared_this);
		catch_all([&]() {
			shared_this->insert_new_crawl();
		});
		crawl_task.run();
	});

	// Monitor transid_max and wake up roots when it changes
	BEESNOTE("tracking transid");
	uint64_t last_transid = 0;
	while (!m_stop_requested) {
		BEESTRACE("Measure current transid");
		catch_all([&]() {
			BEESTRACE("calling transid_max_nocache");
			m_transid_re.update(transid_max_nocache());
		});

		const auto new_transid = m_transid_re.count();
		if (new_transid != last_transid) {
			// Don't hold root FDs open too long.
			// The open FDs prevent snapshots from being deleted.
			// cleaner_kthread just keeps skipping over the open dir and all its children.
			// Even open files are a problem if they're big enough.
			// Always run this even if we have no worker threads.
			clear_caches();

			// Insert new roots and restart crawl_more.
			// Don't run this if we have no worker threads.
			crawl_new.run();
		}
		last_transid = new_transid;

		const auto poll_time = max(BEES_TRANSID_POLL_INTERVAL, m_transid_re.seconds_for(1));
		BEESLOGDEBUG("Polling " << poll_time << "s for next transid " << m_transid_re);
		BEESNOTE("waiting " << poll_time << "s for next transid " << m_transid_re);
		unique_lock<mutex> lock(m_stop_mutex);
		if (m_stop_requested) {
			BEESLOGDEBUG("Stop requested in crawl thread");
			break;
		}
		m_stop_condvar.wait_for(lock, chrono::duration<double>(poll_time));
	}
}

void
BeesRoots::writeback_thread()
{
	while (true) {
		BEESNOTE("idle, " << (m_crawl_clean != m_crawl_dirty ? "dirty" : "clean"));

		catch_all([&]() {
			BEESNOTE("saving crawler state");
			state_save();
		});

		unique_lock<mutex> lock(m_stop_mutex);
		if (m_stop_requested) {
			BEESLOGDEBUG("Stop requested in writeback thread");
			catch_all([&]() {
				BEESNOTE("flushing crawler state");
				state_save();
			});
			return;
		}
		m_stop_condvar.wait_for(lock, chrono::duration<double>(BEES_WRITEBACK_INTERVAL));
	}
}

void
BeesRoots::insert_root(const BeesCrawlState &new_bcs)
{
	unique_lock<mutex> lock(m_mutex);
	if (!m_root_crawl_map.count(new_bcs.m_root)) {
		auto new_bcp = make_shared<BeesCrawl>(m_ctx, new_bcs);
		auto new_pair = make_pair(new_bcs.m_root, new_bcp);
		m_root_crawl_map.insert(new_pair);
		++m_crawl_dirty;
	}
	auto found = m_root_crawl_map.find(new_bcs.m_root);
	THROW_CHECK0(runtime_error, found != m_root_crawl_map.end());
	found->second->deferred(false);
}

void
BeesRoots::insert_new_crawl()
{
	BEESNOTE("adding crawlers for new subvols and removing crawlers for removed subvols");

	BeesCrawlState new_bcs;
	// Avoid a wasted loop iteration by starting from root 5
	new_bcs.m_root = BTRFS_FS_TREE_OBJECTID;
	new_bcs.m_min_transid = transid_min();
	new_bcs.m_max_transid = transid_max();

	unique_lock<mutex> lock(m_mutex);
	set<uint64_t> excess_roots;
	for (const auto &i : m_root_crawl_map) {
		BEESTRACE("excess_roots.insert(" << i.first << ")");
		excess_roots.insert(i.first);
	}
	lock.unlock();

	while (new_bcs.m_root) {
		BEESTRACE("excess_roots.erase(" << new_bcs.m_root << ")");
		excess_roots.erase(new_bcs.m_root);
		BEESTRACE("insert_root(" << new_bcs << ")");
		insert_root(new_bcs);
		BEESCOUNT(crawl_create);
		BEESTRACE("next_root(" << new_bcs.m_root << ")");
		new_bcs.m_root = next_root(new_bcs.m_root);
	}

	for (const auto &i : excess_roots) {
		new_bcs.m_root = i;
		BEESTRACE("crawl_state_erase(" << new_bcs << ")");
		crawl_state_erase(new_bcs);
	}

	BEESNOTE("rebuilding crawl map");
	BEESTRACE("rebuilding crawl map");

	lock.lock();
	THROW_CHECK0(runtime_error, m_scanner);

	// Work from a copy because BeesCrawl might change the world under us
	const auto crawl_map_copy = m_root_crawl_map;
	lock.unlock();

	// Nothing to crawl?  Seems suspicious...
	if (m_root_crawl_map.empty()) {
		BEESLOGINFO("crawl map is empty!");
	}

	// We'll send an empty map to the scanner anyway, maybe we want it to stop
	m_scanner->next_transid(crawl_map_copy);
}

void
BeesRoots::state_load()
{
	BEESNOTE("loading crawl state");
	BEESLOGINFO("loading crawl state");

	string crawl_data = m_crawl_state_file.read();

	for (auto line : split("\n", crawl_data)) {
		BEESLOGDEBUG("Read line: " << line);
		map<string, uint64_t> d;
		auto words = split(" ", line);
		for (auto it = words.begin(); it < words.end(); ++it) {
			auto it1 = it;
			++it;
			THROW_CHECK1(out_of_range, words.size(), it < words.end());
			string key = *it1;
			uint64_t val = from_hex(*it);
			BEESTRACE("key " << key << " val " << val);
			auto result = d.insert(make_pair(key, val));
			THROW_CHECK0(runtime_error, result.second);
		}
		BeesCrawlState loaded_state;
		loaded_state.m_root        = d.at("root");
		loaded_state.m_objectid    = d.at("objectid");
		loaded_state.m_offset      = d.at("offset");
		loaded_state.m_min_transid = d.count("gen_current") ? d.at("gen_current") : d.at("min_transid");
		loaded_state.m_max_transid = d.count("gen_next") ? d.at("gen_next") : d.at("max_transid");
		if (d.count("started")) {
			loaded_state.m_started = d.at("started");
		}
		BEESLOGDEBUG("loaded_state " << loaded_state);
		if (loaded_state.m_min_transid == numeric_limits<uint64_t>::max()) {
			BEESLOGWARN("WARNING: root " << loaded_state.m_root << ": bad min_transid " << loaded_state.m_min_transid << ", resetting to 0");
			loaded_state.m_min_transid = 0;
			BEESCOUNT(bug_bad_min_transid);
		}
		if (loaded_state.m_max_transid == numeric_limits<uint64_t>::max()) {
			BEESLOGWARN("WARNING: root " << loaded_state.m_root << ": bad max_transid " << loaded_state.m_max_transid << ", resetting to " << loaded_state.m_min_transid);
			loaded_state.m_max_transid = loaded_state.m_min_transid;
			BEESCOUNT(bug_bad_max_transid);
		}
		insert_root(loaded_state);
	}
}

BeesRoots::BeesRoots(shared_ptr<BeesContext> ctx) :
	m_ctx(ctx),
	m_root_fetcher(ctx->root_fd()),
	m_crawl_state_file(ctx->home_fd(), crawl_state_filename()),
	m_crawl_thread("crawl_transid"),
	m_writeback_thread("crawl_writeback")
{
}

void
BeesRoots::start()
{
	m_crawl_thread.exec([&]() {
		// Measure current transid before creating any crawlers
		catch_all([&]() {
			m_transid_re.update(transid_max_nocache());
		});

		// Make sure we have a full complement of crawlers
		catch_all([&]() {
			state_load();
		});

		m_writeback_thread.exec([&]() {
			writeback_thread();
		});
		crawl_thread();
	});
}

void
BeesRoots::stop_request()
{
	BEESLOGDEBUG("BeesRoots stop requested");
	BEESNOTE("stopping BeesRoots");
	unique_lock<mutex> lock(m_stop_mutex);
	m_stop_requested = true;
	m_stop_condvar.notify_all();
	lock.unlock();
}

void
BeesRoots::stop_wait()
{
	// Stop crawl writeback first because we will break progress
	// state tracking when we cancel the TaskMaster queue
	BEESLOGDEBUG("Waiting for crawl writeback");
	BEESNOTE("waiting for crawl_writeback thread");
	m_writeback_thread.join();

	BEESLOGDEBUG("Waiting for crawl thread");
	BEESNOTE("waiting for crawl_thread thread");
	m_crawl_thread.join();

	BEESLOGDEBUG("BeesRoots stopped");
}

Fd
BeesRoots::open_root_nocache(uint64_t rootid)
{
	BEESTRACE("open_root_nocache " << rootid);
	BEESNOTE("open_root_nocache " << rootid);

	// Stop recursion at the root of the filesystem tree
	if (rootid == BTRFS_FS_TREE_OBJECTID) {
		return m_ctx->root_fd();
	}

	// Find backrefs for this rootid and follow up to root
	BtrfsIoctlSearchKey sk;
	sk.tree_id = BTRFS_ROOT_TREE_OBJECTID;
	sk.min_objectid = sk.max_objectid = rootid;
	sk.min_type = sk.max_type = BTRFS_ROOT_BACKREF_KEY;

	BEESTRACE("sk " << sk);
	while (sk.min_objectid <= rootid) {
		sk.do_ioctl(m_ctx->root_fd());

		if (sk.m_result.empty()) {
			break;
		}

		for (auto i : sk.m_result) {
			sk.next_min(i, BTRFS_ROOT_BACKREF_KEY);
			if (i.type == BTRFS_ROOT_BACKREF_KEY && i.objectid == rootid) {
				const auto dirid = btrfs_get_member(&btrfs_root_ref::dirid, i.m_data);
				const auto name_len = btrfs_get_member(&btrfs_root_ref::name_len, i.m_data);
				const auto name_start = sizeof(struct btrfs_root_ref);
				const auto name_end = name_len + name_start;
				THROW_CHECK2(runtime_error, i.m_data.size(), name_end, i.m_data.size() >= name_end);
				const string name(i.m_data.data() + name_start, i.m_data.data() + name_end);

				const auto parent_rootid = i.offset;
				// BEESLOG("parent_rootid " << parent_rootid << " dirid " << dirid << " name " << name);
				BEESTRACE("parent_rootid " << parent_rootid << " dirid " << dirid << " name " << name);
				BEESCOUNT(root_parent_open_try);
				Fd parent_fd = open_root(parent_rootid);
				if (!parent_fd) {
					BEESLOGTRACE("no parent_fd");
					BEESCOUNT(root_parent_open_fail);
					continue;
				}
				BEESCOUNT(root_parent_open_ok);

				if (dirid != BTRFS_FIRST_FREE_OBJECTID) {
					BEESTRACE("dirid " << dirid << " root " << rootid << " INO_PATH");
					BtrfsIoctlInoPathArgs ino(dirid);
					if (!ino.do_ioctl_nothrow(parent_fd)) {
						BEESLOGINFO("dirid " << dirid << " inode path lookup failed in parent_fd " << name_fd(parent_fd) << ": " << strerror(errno));
						BEESCOUNT(root_parent_path_fail);
						continue;
					}
					if (ino.m_paths.empty()) {
						BEESLOGINFO("dirid " << dirid << " inode has no paths in parent_fd " << name_fd(parent_fd));
						BEESCOUNT(root_parent_path_empty);
						continue;
					}
					// Theoretically there is only one, so don't bother looping.
					BEESTRACE("dirid " << dirid << " path " << ino.m_paths.at(0));
					parent_fd = openat(parent_fd, ino.m_paths.at(0).c_str(), FLAGS_OPEN_DIR);
					if (!parent_fd) {
						BEESLOGTRACE("no parent_fd from dirid");
						BEESCOUNT(root_parent_path_open_fail);
						continue;
					}
				}
				// BEESLOG("openat(" << name_fd(parent_fd) << ", " << name << ")");
				BEESTRACE("openat(" << name_fd(parent_fd) << ", " << name << ")");
				Fd rv = openat(parent_fd, name.c_str(), FLAGS_OPEN_DIR);
				if (!rv) {
					BEESLOGTRACE("open failed for name " << name << ": " << strerror(errno));
					BEESCOUNT(root_open_fail);
					continue;
				}
				BEESCOUNT(root_found);

				// Verify correct root ID
				// Throw exceptions here because these are very rare events
				// and unlike the file open case, we don't have alternatives to try
				auto new_root_id = btrfs_get_root_id(rv);
				THROW_CHECK2(runtime_error, new_root_id, rootid, new_root_id == rootid);
				Stat st(rv);
				THROW_CHECK1(runtime_error, st.st_ino, st.st_ino == BTRFS_FIRST_FREE_OBJECTID);
				// BEESLOGDEBUG("open_root_nocache " << rootid << ": " << name_fd(rv));

				BEESCOUNT(root_ok);
				return rv;
			}
		}
	}
	BEESLOGDEBUG("No path for rootid " << rootid);
	BEESCOUNT(root_notfound);
	return Fd();
}

Fd
BeesRoots::open_root(uint64_t rootid)
{
	// Ignore some of the crap that comes out of LOGICAL_INO
	if (rootid == BTRFS_ROOT_TREE_OBJECTID) {
		return Fd();
	}

	return m_ctx->fd_cache()->open_root(rootid);
}

bool
BeesRoots::is_root_ro(uint64_t root)
{
	// If we are not working around btrfs send, all roots are rw to us
	if (!m_workaround_btrfs_send) {
		return false;
	}

	BEESTRACE("checking subvol flags on root " << root);

	const auto item = m_root_fetcher.root(root);
	// If we can't access the subvol's root item...guess it's ro?
	if (!item || item.root_flags() & BTRFS_ROOT_SUBVOL_RDONLY) {
		return true;
	}
	return false;
}

uint64_t
BeesRoots::next_root(uint64_t root)
{
	BEESNOTE("Next root from " << root);
	BEESTRACE("Next root from " << root);

	// BTRFS_FS_TREE_OBJECTID has no backref keys so we can't find it that way
	if (root < BTRFS_FS_TREE_OBJECTID) {
		// BEESLOGDEBUG("First root is BTRFS_FS_TREE_OBJECTID = " << BTRFS_FS_TREE_OBJECTID);
		return BTRFS_FS_TREE_OBJECTID;
	}

	BtrfsIoctlSearchKey sk;
	sk.tree_id = BTRFS_ROOT_TREE_OBJECTID;
	sk.min_type = sk.max_type = BTRFS_ROOT_BACKREF_KEY;
	sk.min_objectid = root + 1;

	while (true) {
		sk.do_ioctl(m_ctx->root_fd());

		if (sk.m_result.empty()) {
			return 0;
		}

		for (auto i : sk.m_result) {
			sk.next_min(i, BTRFS_ROOT_BACKREF_KEY);
			if (i.type == BTRFS_ROOT_BACKREF_KEY) {
				// BEESLOGDEBUG("Found root " << i.objectid << " parent " << i.offset << " transid " << i.transid);
				return i.objectid;
			}
		}
	}
}

Fd
BeesRoots::open_root_ino_nocache(uint64_t root, uint64_t ino)
{
	BEESTRACE("opening root " << root << " ino " << ino);

	// Check the tmpfiles map first
	{
		unique_lock<mutex> lock(m_tmpfiles_mutex);
		auto found = m_tmpfiles.find(BeesFileId(root, ino));
		if (found != m_tmpfiles.end()) {
			BEESCOUNT(open_tmpfile);
			return found->second;
		}
	}

	Fd root_fd = open_root(root);
	if (!root_fd) {
		BEESCOUNT(open_no_root);
		return root_fd;
	}

	BEESTOOLONG("open_root_ino(root " << root << ", ino " << ino << ")");

	BEESTRACE("looking up ino " << ino);
	BtrfsIoctlInoPathArgs ipa(ino);
	if (!ipa.do_ioctl_nothrow(root_fd)) {
		if (errno == ENOENT) {
			BEESCOUNT(open_lookup_enoent);
		} else {
			BEESLOGINFO("Lookup root " << root << " ino " << ino << " failed: " << strerror(errno));
			BEESCOUNT(open_lookup_error);
		}
		return Fd();
	}

	BEESTRACE("searching paths for root " << root << " ino " << ino);
	Fd rv;
	if (ipa.m_paths.empty()) {
		BEESLOGWARN("No paths for root " << root << " ino " << ino);
		BEESCOUNT(open_lookup_empty);
	}
	BEESCOUNT(open_lookup_ok);

	for (auto file_path : ipa.m_paths) {
		BEESTRACE("Looking up root " << root << " ino " << ino << " in dir " << name_fd(root_fd) << " path " << file_path);
		BEESCOUNT(open_file);
		// Just open file RO.  root can do the dedupe ioctl without
		// opening in write mode, and if we do open in write mode,
		// we can't exec the file while we have it open.
		const char *fp_cstr = file_path.c_str();
		rv = openat(root_fd, fp_cstr, FLAGS_OPEN_FILE);
		if (!rv) {
			// errno == ENOENT is the most common error case.
			// No need to report it.
			if (errno == ENOENT) {
				BEESCOUNT(open_fail_enoent);
			} else {
				BEESLOGWARN("Could not open path '" << file_path << "' at root " << root << " " << name_fd(root_fd) << ": " << strerror(errno));
				BEESCOUNT(open_fail_error);
			}
			continue;
		}

		// Correct inode?
		Stat file_stat(rv);
		if (file_stat.st_ino != ino) {
			BEESLOGWARN("Opening " << name_fd(root_fd) << "/" << file_path << " found wrong inode " << file_stat.st_ino << " instead of " << ino);
			rv = Fd();
			BEESCOUNT(open_wrong_ino);
			break;
		}

		// Correct root?
		auto file_root = btrfs_get_root_id(rv);
		if (file_root != root) {
			BEESLOGWARN("Opening " << name_fd(root_fd) << "/" << file_path << " found wrong root " << file_root << " instead of " << root);
			rv = Fd();
			BEESCOUNT(open_wrong_root);
			break;
		}

		// Same filesystem?
		Stat root_stat(root_fd);
		if (root_stat.st_dev != file_stat.st_dev) {
			BEESLOGWARN("Opening root " << name_fd(root_fd) << " path " << file_path << " found path st_dev " << file_stat.st_dev << " but root st_dev is " << root_stat.st_dev);
			rv = Fd();
			BEESCOUNT(open_wrong_dev);
			break;
		}

		// The kernel rejects dedupe requests with
		// src and dst that have different datasum flags
		// (datasum is a flag in the inode).
		//
		// We can detect the common case where a file is
		// marked with nodatacow (which implies nodatasum).
		// nodatacow files are arguably out of scope for dedupe,
		// since dedupe would just make them datacow again.
		// To handle these we pretend we couldn't open them.
		//
		// A less common case is nodatasum + datacow files.
		// Those are availble for dedupe but we have to solve
		// some other problems before we can dedupe them.  They
		// require a separate hash table namespace from datasum
		// + datacow files, and we have to create nodatasum
		// temporary files when we rewrite extents.
		//
		// FIXME:  the datasum flag is scooped up by
		// TREE_SEARCH_V2 during crawls.  We throw the inode
		// items away when we should be examining them for the
		// nodatasum flag.

		int attr = ioctl_iflags_get(rv);
		if (attr & FS_NOCOW_FL) {
			BEESLOGWARN("Opening " << name_fd(rv) << " found FS_NOCOW_FL flag in " << to_hex(attr));
			rv = Fd();
			BEESCOUNT(open_wrong_flags);
			break;
		}

		BEESCOUNT(open_hit);
		return rv;
	}

	// All of the paths we tried were wrong.
	BEESCOUNT(open_no_path);
	return Fd();
}

Fd
BeesRoots::open_root_ino(uint64_t root, uint64_t ino)
{
	return m_ctx->fd_cache()->open_root_ino(root, ino);
}

RateEstimator &
BeesRoots::transid_re()
{
	return m_transid_re;
}

void
BeesRoots::insert_tmpfile(Fd fd)
{
	BeesFileId fid(fd);
	unique_lock<mutex> lock(m_tmpfiles_mutex);
	auto rv = m_tmpfiles.insert(make_pair(fid, fd));
	THROW_CHECK1(runtime_error, fd, rv.second);
}

void
BeesRoots::erase_tmpfile(Fd fd)
{
	BeesFileId fid(fd);
	unique_lock<mutex> lock(m_tmpfiles_mutex);
	auto found = m_tmpfiles.find(fid);
	THROW_CHECK1(runtime_error, fd, found != m_tmpfiles.end());
	m_tmpfiles.erase(found);
}

BeesCrawl::BeesCrawl(shared_ptr<BeesContext> ctx, BeesCrawlState initial_state) :
	m_ctx(ctx),
	m_state(initial_state),
	m_btof(ctx->root_fd())
{
	m_btof.scale_size(1);
	m_btof.tree(initial_state.m_root);
	m_btof.type(BTRFS_EXTENT_DATA_KEY);
}

bool
BeesCrawl::next_transid()
{
	const auto roots = m_ctx->roots();
	const auto next_transid = roots->transid_max();
	auto crawl_state = get_state_end();

	// If we are already at transid_max then we are still finished
	m_finished = crawl_state.m_max_transid >= next_transid;

	if (m_finished) {
		m_deferred = true;
		BEESLOGINFO("Crawl finished " << crawl_state);
	} else {
		// Log performance stats from the old crawl
		const auto current_time = time(NULL);

		// Start new crawl
		crawl_state.m_min_transid = crawl_state.m_max_transid;
		crawl_state.m_max_transid = next_transid;
		crawl_state.m_objectid = 0;
		crawl_state.m_offset = 0;
		crawl_state.m_started = current_time;
		BEESCOUNT(crawl_restart);
		set_state(crawl_state);
		m_deferred = false;
		BEESLOGINFO("Crawl started " << crawl_state);
	}

	return !m_finished;
}

bool
BeesCrawl::fetch_extents()
{
	BEESTRACE("fetch_extents " << get_state_end());
	BEESNOTE("fetch_extents " << get_state_end());
	// insert_root will undefer us.  Until then, nothing.
	if (m_deferred) {
		return false;
	}

	const auto old_state = get_state_end();

	// We can't scan an empty transid interval.
	if (m_finished || old_state.m_max_transid <= old_state.m_min_transid) {
		return next_transid();
	}

	// Check for btrfs send workaround: don't scan RO roots at all, pretend
	// they are just empty.  We can't free any space there, and we
	// don't have the necessary analysis logic to be able to use
	// them as dedupe src extents (yet).
	BEESTRACE("is_root_ro(" << old_state.m_root << ")");
	if (m_ctx->is_root_ro(old_state.m_root)) {
		BEESLOGDEBUG("WORKAROUND: skipping scan of RO root " << old_state.m_root);
		BEESCOUNT(root_workaround_btrfs_send);
		// We would call next_transid() here, but we want to do a few things differently.
		// We immediately defer further crawling on this subvol.
		// We track max_transid if the subvol scan has never started.
		// We postpone the started timestamp since we haven't started.
		auto crawl_state = old_state;
		if (crawl_state.m_objectid == 0) {
			// This will keep the max_transid up to date so if the root
			// is ever switched back to read-write, it won't trigger big
			// expensive in-kernel searches for ancient transids.
			// If the root is made RO while crawling is in progress, we will
			// have the big expensive in-kernel searches (same as if we have
			// been not running for a long time).
			// Don't allow transid_max to ever move backwards.
			const auto roots = m_ctx->roots();
			const auto next_transid = roots->transid_max();
			const auto current_time = time(NULL);
			crawl_state.m_max_transid = max(next_transid, crawl_state.m_max_transid);
			// Move the start time forward too, since we have not started crawling yet.
			crawl_state.m_started = current_time;
			set_state(crawl_state);
		}
		// Mark this root deferred so we won't see it until the next transid cycle
		m_deferred = true;
		return false;
	}

	BEESNOTE("crawling " << old_state);

	// Find an extent data item in this subvol in the transid range
	BEESTRACE("looking for new objects " << old_state);
	// Don't set max_transid to m_max_transid here.	 See crawl_one_extent.
	m_btof.transid(old_state.m_min_transid);
	if (catch_all([&]() {
		m_next_extent_data = m_btof.lower_bound(old_state.m_objectid);
	})) {
		// Whoops that didn't work.  Stop scanning this subvol, move on to the next.
		m_deferred = true;
		return false;
	}
	if (!m_next_extent_data) {
		// Ran out of data in this subvol and transid.
		// Try to restart immediately if more transids are available.
		return next_transid();
	}
	auto new_state = old_state;
	new_state.m_objectid = max(m_next_extent_data.objectid() + 1, m_next_extent_data.objectid());
	new_state.m_offset = 0;
	set_state(new_state);
	return true;
}

void
BeesCrawl::fetch_extents_harder()
{
	BEESNOTE("fetch_extents_harder " << get_state_end());
	BEESTRACE("fetch_extents_harder " << get_state_end());
	while (!m_next_extent_data) {
		const bool progress_made = fetch_extents();
		if (!progress_made) {
			return;
		}
	}
}

BeesFileRange
BeesCrawl::bti_to_bfr(const BtrfsTreeItem &bti) const
{
	if (!bti) {
		return BeesFileRange();
	}
	return BeesFileRange(
		BeesFileId(get_state_end().m_root, bti.objectid()),
		bti.offset(),
		bti.offset() + bti.file_extent_logical_bytes()
	);
}

BeesFileRange
BeesCrawl::peek_front()
{
	unique_lock<mutex> lock(m_mutex);
	fetch_extents_harder();
	return bti_to_bfr(m_next_extent_data);
}

BeesFileRange
BeesCrawl::pop_front()
{
	unique_lock<mutex> lock(m_mutex);
	fetch_extents_harder();
	BtrfsTreeItem rv;
	swap(rv, m_next_extent_data);
	return bti_to_bfr(rv);
}

BeesCrawlState
BeesCrawl::get_state_begin()
{
	return m_state.begin();
}

BeesCrawlState
BeesCrawl::get_state_end() const
{
	return m_state.end();
}

ProgressTracker<BeesCrawlState>::ProgressHolder
BeesCrawl::hold_state(const BeesCrawlState &bcs)
{
	return m_state.hold(bcs);
}

void
BeesCrawl::set_state(const BeesCrawlState &bcs)
{
	m_state.hold(bcs);
	m_ctx->roots()->crawl_state_set_dirty();
}

void
BeesCrawl::deferred(bool def_setting)
{
	unique_lock<mutex> lock(m_state_mutex);
	m_deferred = def_setting;
}
