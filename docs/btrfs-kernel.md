Recommended Kernel Version for bees
===================================

First, a warning that is not specific to bees:

> **Kernel 5.1, 5.2, and 5.3 should not be used with btrfs due to a
severe regression that can lead to fatal metadata corruption.**
This issue is fixed in kernel 5.4.14 and later.

**Recommended kernel versions for bees are 4.19, 5.4, 5.10, 5.11, or 5.12,
with recent LTS and -stable updates.**  The latest released kernel as
of this writing is 5.18.18.

4.14, 4.9, and 4.4 LTS kernels with recent updates are OK with
some issues.  Older kernels will be slower (a little slower or a lot
slower depending on which issues are triggered).  Not all fixes are
backported.

Obsolete non-LTS kernels have a variety of unfixed issues and should
not be used with btrfs.  For details see the table below.

bees requires btrfs kernel API version 4.2 or higher, and does not work
on older kernels.

bees will detect and use btrfs kernel API up to version 4.15 if present.
In some future bees release, this API version may become mandatory.




Kernel Bug Tracking Table
-------------------------

These bugs are particularly popular among bees users, though not all are specifically relevant to bees:

| First bad kernel | Last bad kernel | Issue Description | Fixed Kernel Versions | Fix Commit
| :---: | :---: | --- | :---: | ---
| - | 4.10 | garbage inserted in read data when reading compressed inline extent followed by a hole | 3.18.89, 4.1.49, 4.4.107, 4.9.71, 4.11 and later | e1699d2d7bf6 btrfs: add missing memset while reading compressed inline extents
| - | 4.14 | spurious warnings from `fs/btrfs/backref.c` in `find_parent_nodes` | 3.16.57, 4.14.29, 4.15.12, 4.16 and later | c8195a7b1ad5 btrfs: remove spurious WARN_ON(ref->count < 0) in find_parent_nodes
| 4.15 | 4.18 | compression ratio and performance regression on bees test corpus | improved in 4.19 | 4.14 performance not fully restored yet
| - | 5.0 | silently corrupted data returned when reading compressed extents around a punched hole (bees dedupes all-zero data blocks with holes which can produce a similar effect to hole punching) | 3.16.70, 3.18.137, 4.4.177, 4.9.165, 4.14.108, 4.19.31, 5.0.4, 5.1 and later | 8e928218780e Btrfs: fix corruption reading shared and compressed extents after hole punching
| - | 5.0 | deadlock when dedupe and rename are used simultaneously on the same files | 5.0.4, 5.1 and later | 4ea748e1d2c9 Btrfs: fix deadlock between clone/dedupe and rename
| - | 5.1 | send failure or kernel crash while running send and dedupe on same snapshot at same time | 5.0.18, 5.1.4, 5.2 and later | 62d54f3a7fa2 Btrfs: fix race between send and deduplication that lead to failures and crashes
| - | 5.2 | alternating send and dedupe results in incremental send failure | 4.9.188, 4.14.137, 4.19.65, 5.2.7, 5.3 and later | b4f9a1a87a48 Btrfs: fix incremental send failure after deduplication
| 4.20 | 5.3 | balance convert to single rejected with error on 32-bit CPUs | 5.3.7, 5.4 and later | 7a54789074a5 btrfs: fix balance convert to single on 32-bit host CPUs
| - | 5.3 | kernel crash due to tree mod log issue #1 (often triggered by bees) | 3.16.79, 4.4.195, 4.9.195, 4.14.147, 4.19.77, 5.2.19, 5.3.4, 5.4 and later | efad8a853ad2 Btrfs: fix use-after-free when using the tree modification log
| - | 5.4 | kernel crash due to tree mod log issue #2 (often triggered by bees) | 3.16.83, 4.4.208, 4.9.208, 4.14.161, 4.19.92, 5.4.7, 5.5 and later | 6609fee8897a Btrfs: fix removal logic of the tree mod log that leads to use-after-free issues
| 5.1 | 5.4 | metadata corruption resulting in loss of filesystem when a write operation occurs while balance starts a new block group.  **Do not use kernel 5.1 with btrfs.**  Kernel 5.2 and 5.3 have workarounds that may detect corruption in progress and abort before it becomes permanent, but do not prevent corruption from occurring.  Also kernel crash due to tree mod log issue #4. | 5.4.14, 5.5 and later | 6282675e6708 btrfs: relocation: fix reloc_root lifespan and access
| - | 5.4 | send performance failure when shared extents have too many references | 4.9.207, 4.14.159, 4.19.90, 5.3.17, 5.4.4, 5.5 and later | fd0ddbe25095 Btrfs: send, skip backreference walking for extents with many references
| 5.0 | 5.5 | dedupe fails to remove the last extent in a file if the file size is not a multiple of 4K | 5.4.19, 5.5.3, 5.6 and later | 831d2fa25ab8 Btrfs: make deduplication with range including the last block work
| 4.5, backported to 3.18.31, 4.1.22, 4.4.4 | 5.5 | `df` incorrectly reports 0 free space while data space is available.  Triggered by changes in metadata size, including those typical of large-scale dedupe.  Occurs more often starting in 5.3 and especially 5.4 | 4.4.213, 4.9.213, 4.14.170, 4.19.102, 5.4.18, 5.5.2, 5.6 and later | d55966c4279b btrfs: do not zero f_bavail if we have available space
| - | 5.5 | kernel crash due to tree mod log issue #3 (often triggered by bees) | 3.16.84, 4.4.214, 4.9.214, 4.14.171, 4.19.103, 5.4.19, 5.5.3, 5.6 and later | 7227ff4de55d Btrfs: fix race between adding and putting tree mod seq elements and nodes
| - | 5.6 | deadlock when enumerating file references to physical extent addresses while some references still exist in deleted subvols | 5.7 and later | 39dba8739c4e btrfs: do not resolve backrefs for roots that are being deleted
| - | 5.6 | deadlock when many extent reference updates are pending and available memory is low | 4.14.177, 4.19.116, 5.4.33, 5.5.18, 5.6.5, 5.7 and later | 351cbf6e4410 btrfs: use nofs allocations for running delayed items
| - | 5.6 | excessive CPU usage in `LOGICAL_INO` and `FIEMAP` ioctl and increased btrfs write latency in other processes when bees translates from extent physical address to list of referencing files and offsets.  Also affects other tools like `duperemove` and `btrfs send` | 5.4.96, 5.7 and later | b25b0b871f20 btrfs: backref, use correct count to resolve normal data refs, plus 3 parent commits.  Some improvements also in earlier kernels.
| - | 5.7 | filesystem becomes read-only if out of space while deleting snapshot | 4.9.238, 4.14.200, 4.19.149, 5.4.69, 5.8 and later | 7c09c03091ac btrfs: don't force read-only after error in drop snapshot
| 5.1 | 5.7 | balance, device delete, or filesystem shrink operations loop endlessly on a single block group without decreasing extent count | 5.4.54, 5.7.11, 5.8 and later | 1dae7e0e58b4 btrfs: reloc: clear DEAD\_RELOC\_TREE bit for orphan roots to prevent runaway balance
| - | 5.8 | deadlock in `TREE_SEARCH` ioctl (core component of bees filesystem scanner), followed by regression in deadlock fix | 4.4.237, 4.9.237, 4.14.199, 4.19.146, 5.4.66, 5.8.10 and later | a48b73eca4ce btrfs: fix potential deadlock in the search ioctl, 1c78544eaa46 btrfs: fix wrong address when faulting in pages in the search ioctl
| 5.7 | 5.10 | kernel crash if balance receives fatal signal e.g. Ctrl-C | 5.4.93, 5.10.11, 5.11 and later | 18d3bff411c8 btrfs: don't get an EINTR during drop_snapshot for reloc
| 5.10 | 5.10 | 20x write performance regression | 5.10.8, 5.11 and later | e076ab2a2ca7 btrfs: shrink delalloc pages instead of full inodes
| 5.4 | 5.11 | spurious tree checker failures on extent ref hash | 5.11.5, 5.12 and later | 1119a72e223f btrfs: tree-checker: do not error out if extent ref hash doesn't match
| - | 5.11 | tree mod log issue #5 | 4.4.263, 4.9.263, 4.14.227, 4.19.183, 5.4.108, 5.10.26, 5.11.9, 5.12 and later | dbcc7d57bffc btrfs: fix race when cloning extent buffer during rewind of an old root
| - | 5.12 | tree mod log issue #6 | 4.14.233, 4.19.191, 5.4.118, 5.10.36, 5.11.20, 5.12.3, 5.13 and later | f9690f426b21 btrfs: fix race when picking most recent mod log operation for an old root
| 4.15 | 5.16 | spurious warnings from `fs/fs-writeback.c` when `flushoncommit` is enabled | 5.15.27, 5.16.13, 5.17 and later | a0f0cf8341e3 btrfs: get rid of warning on transaction commit when using flushoncommit
| - | 5.17 | crash during device removal can make filesystem unmountable | 5.15.54, 5.16.20, 5.17.3, 5.18 and later | bbac58698a55 btrfs: remove device item and update super block in the same transaction
| - | 5.18 | wrong superblock num_devices makes filesystem unmountable | 4.14.283, 4.19.247, 5.4.198, 5.10.121, 5.15.46, 5.17.14, 5.18.3, 5.19 and later | d201238ccd2f btrfs: repair super block num_devices automatically
| 5.18 | 5.19 | parent transid verify failed during log tree replay after a crash during a rename operation | 5.18.18, 5.19.2, 6.0 and later | 723df2bcc9e1 btrfs: join running log transaction when logging new name
| 5.4 | - | kernel hang when multiple threads are running `LOGICAL_INO` and dedupe ioctl | - | workaround: reduce bees thread count to 1 with `-c1`

"Last bad kernel" refers to that version's last stable update from
kernel.org.  Distro kernels may backport additional fixes.  Consult
your distro's kernel support for details.

When the same version appears in both "last bad kernel" and "fixed kernel
version" columns, it means the bug appears in the `.0` release and is
fixed in the stated `.y` release.  e.g.  a "last bad kernel" of 5.4 and
a "fixed kernel version" of 5.4.14 has the bug in kernel versions 5.4.0
through 5.4.13 inclusive.

A "-" for "first bad kernel" indicates the bug has been present since
the relevant feature first appeared in btrfs.

A "-" for "last bad kernel" indicates the bug has not yet been fixed as
of 5.18.18.

In cases where issues are fixed by commits spread out over multiple
kernel versions, "fixed kernel version" refers to the version that
contains all components of the fix.


Workarounds for known kernel bugs
---------------------------------

* **Hangs with high worker thread counts**:  On kernels newer than
  5.4, multiple threads running `LOGICAL_INO` and dedupe ioctls
  at the same time can lead to a kernel hang.  The workaround is
  to reduce the thread count to 1 with `-c1`.

* **Slow backrefs** (aka toxic extents):  Under certain conditions,
  if the number of references to a single shared extent grows too
  high, the kernel consumes more and more CPU while also holding locks
  that delay write access to the filesystem.  bees avoids this bug
  by measuring the time the kernel spends performing `LOGICAL_INO`
  operations and permanently blacklisting any extent or hash involved
  where the kernel starts to get slow.  In the bees log, such blocks
  are labelled as 'toxic' hash/block addresses.  Toxic extents are
  rare (about 1 in 100,000 extents become toxic), but toxic extents can
  become 8 orders of magnitude more expensive to process than the fastest
  non-toxic extents.  This seems to affect all dedupe agents on btrfs;
  at this time of writing only bees has a workaround for this bug.

  This workaround is less necessary for kernels 5.4.96, 5.7 and later,
  though it can still take 2 ms of CPU to resolve each extent ref on a
  fast machine on a large, heavily fragmented file.

* **dedupe breaks `btrfs send` in old kernels**.  The bees option
  `--workaround-btrfs-send` prevents any modification of read-only subvols
  in order to avoid breaking `btrfs send`.

  This workaround is no longer necessary to avoid kernel crashes
  and send performance failure on kernel 4.9.207, 4.14.159, 4.19.90,
  5.3.17, 5.4.4, 5.5 and later; however, some conflict between send
  and dedupe still remains, so the workaround is still useful.

  `btrfs receive` is not and has never been affected by this issue.

Unfixed kernel bugs
-------------------

As of 5.18.18:

* **The kernel does not permit `btrfs send` and dedupe to run at the
  same time**.  Recent kernels no longer crash, but now refuse one
  operation with an error if the other operation was already running.

  bees has not been updated to handle the new dedupe behavior optimally.
  Optimal behavior is to defer dedupe operations when send is detected,
  and resume after the send is finished.  Current bees behavior is to
  complain loudly about each individual dedupe failure in log messages,
  and abandon duplicate data references in the snapshot that send is
  processing.  A future bees version shall have better handling for
  this situation.

  Workaround:  send `SIGSTOP` to bees, or terminate the bees process,
  before running `btrfs send`.

  This workaround is not strictly required if snapshot is deleted after
  sending.  In that case, any duplicate data blocks that were not removed
  by dedupe will be removed by snapshot delete instead.  The workaround
  still saves some IO.

  `btrfs receive` is not affected by this issue.
