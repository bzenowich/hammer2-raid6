# hammer2-raid6 code review — October 2026

Date: 2026-10-10. Tree: `035b1b7` plus the uncommitted whole-buffer fix in
`local_hammer2_io.c` (§1). Scope: the v3 RAID6 code in `src/sys/local_hammer2_*.c`
(about 8k of the 29k lines differ from upstream HAMMER2), the tests in `tests/v3`, and `docs/`.

## How the review was done

- Five read-only reviewers each took one area, and I merged their findings:
  1. allocator and parity write path;
  2. dio and read paths;
  3. flush, mount and failure handling;
  4. resilver, scrub and bulkfree;
  5. structure, tests and docs.
- Findings that two reviewers reported independently are merged.
- Every finding carries one of these tags:
  - **[V]** I re-read the code myself and confirmed it.
  - **[V2]** Two reviewers reported it independently and the code quotes match, but I did not re-check it myself.
  - **[R]** A reviewer traced it, but nobody has reproduced it.
  - **[U]** Unverified trigger or reachability.
- No finding except §1 has been reproduced on h2dev or the Pi yet. §9 lists the cheap repro tests to write first.
- Line numbers are from `src/sys/` at the tree above.

---

## 1. Fixed today: new data blocks on disks 1..N-1 were read before every write

**Found from the hardware.** On the Pi, array position 0 always ran at half the write
rate of the other disks. Swapping `da0` and `da1` in the array moved the slow rate to
`da1`, so the cause was the position, not the device.

**Measurement.** devstat read/write split while copying 256 MB on h2dev:

| disk | reads before | writes before | reads after | writes after |
|---|---|---|---|---|
| vbd1 (idx 0) | 0 MB | 132 MB | 0 MB | 131 MB |
| vbd2–4 | 124–128 MB each | 137 MB | **0 MB** | 137 MB |

**Cause.** `_hammer2_io_getblk` (`local_hammer2_io.c:433`, and the same test at `:412`)
decides that a NEW/NEWNZ request covers the whole buffer with
`dio->pbase == (lbase & ~RADIX)`. A v3 DATA/DIRENT dio's `pbase` carries
`disk_idx << 56` in its top bits (`hammer2_dio_key`), so the test was true only for
disk 0. On every other disk, each fresh 64 KB block was read from the device
(`cluster_readx` with read-ahead) and then zeroed.

**Fix.** The test is now `dio->psize == lsize`. `hammer2_io_alloc` already asserts that
the request lies inside one psize block. With the fix:

- total write I/O is 2.1× the user data (RAID6 2+2 ideal is 2.0×);
- the full suite (A–N) passes 123/0 on h2dev.

**The same bug class survives for sub-64 KB writes**; see P1.

---

## 2. Reliability: critical

### R1. The volume header can commit a TXG before the other disks have flushed it [V]
**Where:** `local_hammer2_flush.c` ~1664–1811 (header loop), ~1559 (device fsync loop).

**What happens.** `VOP_FSYNC` on a devfs vnode is only `vfsync()` and sends no cache
flush. The only cache flush covering data, P/Q, zero columns, metadata mirrors and the
space map is the `BUF_CMD_FLUSH` inside the header loop. That loop runs per disk:

1. FLUSH(d), then wait;
2. write header(d);
3. async FLUSH(d);
4. move to the next disk.

So disk 0's header for TXG T+1 is durable before disk 3 has flushed its T+1 columns.
`docs/volhdr_quorum.md` describes the opposite order: flush every disk, then the header loop.

**Failure.** A power cut after headers have reached disks 0–2:

- Quorum mounts T+1.
- Disk 3 holds stale columns, parity and mirror copies, and is trusted (R2c).
- Block CRCs catch bad data columns. Missing parity is silent until a second failure loses data.

**Fix (three phases):**
1. FLUSH every writable member in parallel and wait. A member whose flush fails is
   auto-failed; abort if that exceeds redundancy.
2. Write all headers asynchronously and wait.
3. FLUSH all members in parallel.

This is also the main sync-latency win (P2).

### R2. Quorum logic: a crash during the header loop can make the array unmountable [V]
**Where:** `local_hammer2_ondisk.c:1040–1182`.

- **(a) Votes count only each disk's newest header.**
  - Each disk contributes only the `rz_txg_seq` of its newest header.
  - Fallback counts `disk_seqs[j] == fallback` exactly.
  - A disk that reached T+1 still holds a valid T header in another zone, but never votes for T.
  - With 4 disks and a crash after 2 headers, the sequences are {T+1, T+1, T, T}. The majority needed is 3, so no sequence wins.
  - Result: ENXIO "no majority" (`:1108`), even with rollback allowed.
- **(b) A one-TXG rollback is refused by default** (`:1123`, `j2_allow_rollback=0`).
  - After a crash with {T+1, T, T, T}, the mount fails until an operator sets the sysctl.
  - The header loop takes hundreds of ms on USB, so this is not a rare window.
  - The design doc treats rollback to seq−1 as the normal path.
- **(c) Lagging disks are only logged** (`:1172–1182`: "needs resilver").
  - The comment says the caller marks them failed. It doesn't: `vfsops.c:1505–1530` only acts on `DISK_FAILED` and absent disks.
  - A disk whose header lags mounts ONLINE and is trusted as a parity source.
- **(d) Smaller problems, only with rollback allowed** [U]:
  - Rollback leaves the orphan T+1 headers in place.
  - Header choice uses `mirror_tid`, not `rz_txg_seq`.
  - `*rootvolzone` is not updated.

**Fix:**
- Record every valid zone's sequence per disk, and count a disk for every sequence it holds.
- Allow `target - fallback == 1` by default.
- Mark each disk below the chosen sequence FAILED (persisted) and require a resilver.
- After a rollback, invalidate the orphan zones before the first commit.

Once R1 is fixed, a T+1 header on any single disk means T+1 is durable everywhere,
so a ZFS-uberblock-style rule (newest valid header wins) becomes possible. That would
remove most of this logic.

### R3. Failure state is set without the lock, not capped, and not persisted, in three places [V]
**Where:**
- read EIO: `local_hammer2_io.c:591–597` (inline `raid_failed[i] = 1; raid_nfailed++`);
- `fail-disk` ioctl: `local_hammer2_ioctl.c` ~1774–1781;
- mount of absent disks: `local_hammer2_vfsops.c:1520–1530` (in memory only).

**What happens.**
- None of these goes through `hammer2_raid6_auto_fail_disk()`, which takes `vollk`, refuses a third failure and persists DISK_FAILED.
- Two readers can count the same disk twice, so `raid_nfailed` can exceed 2.
- A transient USB read error drops a disk for writes while every header still says ONLINE. After a remount it is trusted with stale columns, and its P/Q are silently wrong.
- An absent disk that was never persisted as failed keeps voting in the quorum (R2), so a later crash can leave a degraded array unmountable.
- Also: a disk auto-failed during the fsync loop is still ONLINE in the `volsync` copy that commits that TXG (`flush.c:862` snapshot) [R].

**Fix.**
- Add one `hammer2_raid6_mark_failed(hmp, i, persist)` and use it everywhere.
- Patch `volsync.raid_config` after the fsync loop, as is already done for `rz_sm_gen`.
- Header-loop flush or write errors should auto-fail the member, not just set `fsync_error` (`flush.c` ~1700, ~1781) [R].

### R4. Partial seals produce parity that depends on garbage, and the full seal reopens the write hole [V]
**Where:** `local_hammer2_ondisk.c` `seal_partial_locked` (~1957–2020) and `row_io` (`:1833`).

**What happens.**
- At a TXG flush, a row still waiting for a reserved column is partially sealed. P/Q are computed with that column taken as zero.
- `row_io` skips zero-filling reserved columns (`if (alloc_mask & (1U << d)) continue;`).
- So the on-disk column still holds whatever the slot's previous tenant left.
- Reconstruction reads every surviving column from disk and does not know a column was "pending, treat as zero".

**Consequences:**
- **(a) Crash before the late column arrives.** The committed column's parity is permanently wrong. A later disk failure rebuilds garbage, and the resilver writes it without verifying.
- **(b) Late column arrives in TXG N+1.** P/Q are rewritten in place over a committed column. A torn write there is exactly the RAID-5 write hole that `newplan.md §5.6` says the design avoids.
- **(c) Column arrives during the partial-seal I/O.** The row is complete but is not resealed until the next flush [R].

Three reviewers flagged this independently.

**Fix (structural):** tag each open row with the flush generation that opened it, and
pack only into rows of the current generation. A row then never mixes TXGs, so a
committed column never shares parity with a later write.

Then either:
- make the flush wait for or force every reserved column of its generation, which removes partial seals; or
- zero-fill every non-arrived column at the partial seal.

The cheapest stopgap is to zero-fill reserved-but-pending columns in `row_io` as well.
That fixes (a) but not (b).

### R5. Rewriting a block in the same TXG after its row sealed panics [V logic, R trigger]
**Where:**
- `local_hammer2_chain.c:1567–1588`: an already-MODIFIED chain keeps its slot; the forced `newmod = 1` for v3 applies only to unmodified chains;
- `local_hammer2_ondisk.c:2147–2156`: `panic("v3 putblk for a row not open")`;
- `:1942`: the row is unlinked before its I/O.

**What happens.** A full row seals and leaves `open_rows` as soon as its last column
arrives. A second putblk of the same chain in the same TXG panics. Possible triggers:
- an O_SYNC rewrite;
- a buffer flushed twice before the syncer runs.

Even without the panic, the column would be overwritten under parity that is already on disk.

**Repro to write:** a sync 4 KB write to A, a write to B (packs into the same row), and
a sync rewrite of A, all within one sync interval.

**Fix:**
- Always `newmod = 1` on v3 DATA/DIRENT. That costs a slot until bulkfree, so rate-limit it.
- Or keep the generation's sealed rows on a list until the flush, and recompute P/Q there (safe only after R4's per-generation rows).

### R6. `raid replace` accepts any disk, including a healthy member [V]
**Where:** `local_hammer2_ioctl.c:1583–1647`.

**What happens.** The ioctl checks only `nlookup`, `vn_isdisk` and `VOP_OPEN`. It never checks:
- that the device is not already a member;
- that it is not mounted or open elsewhere;
- that it is at least as large as the failed member.

A typo such as `raid replace da3 da1` makes the resilver write disk 3's columns over
healthy `da1`. That is silent loss of a second column in every row. An undersized disk
fails only when the resilver reaches its end, possibly hours in.

**Fix:** under `rebuild_lk`, reject the device if:
- it is a current member (same `devvp`, `si_disk` or `rdev`);
- `vcount(new_devvp) > 0` or `vfs_mountedon()` is true;
- its size is smaller than `volumes[i].size`.

### R7. The `vinvalbuf` panic from 035b1b7 still exists on other paths [V for fail-disk]
**Where:**
- `fail-disk` ioctl: runs `vinvalbuf` + `VOP_CLOSE` with no `rebuild_lk`;
- `flush.c:1551–1555`: `vinvalbuf` on a non-writable member, no lock;
- getblk users that race the gone task without the shared lock [R]:
  - `hammer2_raid6_sm_write`, called before the header loop takes the lock;
  - the degraded preempt `getblk` on the failed devvp (`io.c:362`, `:455`);
- `disk_gone_hmp` (`vfsops.c:468–494`) does not re-validate `dev`/`devvp` after sleeping on the lock [R]. A replace finishing in that window could get the new disk closed.

**Fix:** one `hammer2_raid6_close_member(hmp, i)` helper that:
- takes `rebuild_lk` exclusive;
- re-checks the member;
- runs `vinvalbuf` + `VOP_CLOSE`.

In addition:
- take the lock shared around `sm_write`;
- give degraded reads kmalloc or pbuf scratch buffers, never `getblk` on a failed devvp (`read_sibling` already works this way).

### R8. A degraded metadata read trusts the first mirror copy without checking it [R]
**Where:** `local_hammer2_raid6.c:794–830` (`metadata_mirror_read`); `local_hammer2_chain.c:5781–5797`.

**What happens.**
- With the primary failed, the first sibling whose `bread` succeeds is returned, with no CRC check.
- Self-heal then refuses to look further, because a comment claims the copy "was already checked".
- All v3 metadata has disk 0 as its primary (P4).
- So with disk 0 dead and a bitrotted copy on disk 1, an inode or subtree fails CHECK while disks 2 and 3 hold good copies. This is the degraded state, exactly when redundancy matters most.

**Fix:** use `hammer2_io_metadata_mirror_heal()` (which tries every copy) in the failed-primary branch.

### R9. Bulkfree can free a slot whose row is still open [U trigger]
**Where:** `local_hammer2_ondisk.c:2596–2600` (`bf_end`); the stuck row at `:2220` is only logged.

**What happens.**
- A reserved column whose chain never puts a block is never released. Suspected source: `hammer2_chain_resize` allocating twice (P5).
- If its committed siblings are later deleted, bulkfree frees the slot while the row is still on `open_rows`.
- The slot is then reallocated, and two rows share one `phys_off`.
- `add_data` matches the stale row first: either a KKASSERT, or silently wrong parity.

**Fix:**
- Keep a per-slot "open" bit, and have `bf_end` skip slots that have it.
- Release reservations on resize.
- Drop rows whose pending columns can never arrive.

### R10. Error paths in `_hammer2_io_getblk` [R]
- **(a)** After a failed I/O the dio keeps `dio->bp`. A thread that was waiting on INPROG then reaches `KKASSERT(dio->bp == NULL)` (`io.c` ~320–339 / 650–662), and the retry leaks `absent_data`. This is an upstream pattern, but RAID6 makes errors much more common.
- **(b)** For a sub-buffer NEW, the read-error path rebuilds the buffer after the range was zeroed (`:531–536` then `:607–632`). A new INDIRECT or FREEMAP block can then carry the old block's contents as live blockrefs.
- **(c)** Self-heal repair writes (`repair_execute`, `:1336–1362`) do not check that the slot still holds the bad bytes. A queued P/Q repair can overwrite a row that bulkfree freed and the allocator reused in between. Parity has no CRC, so this is silent.

**Fix.**
- (a) Make the error sticky, or `brelse` the leftover buffer when taking INPROG.
- (b) Re-apply the NEW zeroing after reconstructing.
- (c) Store the bad column's hash at enqueue and compare it before writing.

### R11. Hangs caused by a hung (not detached) USB disk [R]
- `rebuild_lk` is held shared across untimed `biowait`s in the header loop.
- `disk_gone_hmp` waits for it exclusive while holding the global `hammer2_mntlk`, on the shared `taskqueue_thread[0]`.
- Every seal (`row_io`, shared) queues behind it.
- P/Q writes run synchronously inside putblk under chain locks (P3), so one hung disk wedges the filesystem instead of being failed.
- Unmount sends FLUSH and `vinvalbuf(V_SAVE)` to auto-failed members that are still open (`ondisk.c:183–205`): minutes per stalled command, with `mntlk` held.

**Fix:**
- Give the gone task a private taskqueue, and drop `mntlk` before taking `rebuild_lk`.
- Hold `rebuild_lk` only around buffer acquisition, not across completion waits.
- At unmount, skip FLUSH and V_SAVE for failed members.

### Minor reliability items
- **`stripe_next_disk` is a signed int** that is incremented on every probe (`hammer2.h:1216`). After 2^31 probes (tens of TB without a remount) it goes negative, producing a negative disk index and undefined shifts. Fix: make it unsigned, or store it modulo ndisks. [V]
- **New rows can be placed on a failed disk** (`ondisk.c:2452–2459` skips only P/Q). Each such NEW then rebuilds a never-used slot from parity and zeroes the result. A sibling read error during that pointless rebuild can auto-fail a second disk. The packing path already skips failed disks. [V2]
- **Scrub can report false "unrepairable" blocks** when bulkfree frees and reuses blocks during a long scrub. Scrub doesn't take `bflock`. The write-back is CRC-gated, so this is a false alarm, not corruption. [R]
- **Scrub repair is weaker than read-path self-heal** (single erasure only). It also writes back the whole 64 KB column when only `[off, off+lsize)` was verified (`io.c:2601–2635`). [V2]
- **Resilver status:** `error` is always reported as 0. Progress only advances on allocated slots. Write errors don't stop the loop. [R]
- **`absent_data` path:** if `devvp` were ever NULL, `io.c:611` would call `getblk(NULL…)`. The path looks unreachable: absent disks get a dummy vnode. Delete the path or assert. [U]

---

## 3. Performance, ranked by expected effect on the Pi

### P1. Sub-64 KB DATA/DIRENT writes still read a 64 KB block and its read-ahead [V2]
**Where:** `local_hammer2_io.c:508–525` (the `lsize != psize` branch).

**What happens.** v3 gives every DATA/DIRENT chain a whole 64 KB column of its own, so
nothing in the buffer is worth reading. Yet every compressed block (LZ4 is the default)
and every small-file block does a synchronous 64 KB `cluster_readx`, plus up to 192 KB
of read-ahead (`hce` = 4), before it is written. A read error there can even auto-fail
the disk.

**Fix:** for v3 Path A dios, treat NEW/NEWNZ as whole-buffer whatever `lsize` is:
`getblk`, then zero all of psize (NEW) or the tail past `lsize` (NEWNZ). This is a few
lines, in the same place as §1.

### P2. Sync latency: per-disk serial flushes and header writes [V]
- The header loop does FLUSH, header write and FLUSH per disk, serially: about 2 round trips × N disks. Unmount does this 4 times.
- `VOP_FSYNC(MNT_WAIT)` drains disk 0 completely before disk 1 starts.
- `sm_write` does one synchronous `bwrite` per page per disk.

**Fix:** the three-phase scheme in R1 (about 3 parallel round trips in total), plus an
`MNT_NOWAIT` pass over all members before the `MNT_WAIT` pass, plus batched `sm_write`
through `bwrite_start`/`bwrite_wait`.

### P3. Synchronous P/Q and mirror writes in putblk, and parity left in the cache [V2]
- **Rows.** The putblk that completes a row runs `seal_row` → `write_row` → `biowait` on P and Q while the caller holds chain locks. Every second 64 KB data write therefore blocks an XOP thread on two USB writes.
- **Metadata.** Each dirty metadata putblk (`io.c:907–913` → `raid6.c:705–781`) writes 3 × 64 KB mirrors synchronously. The primary is only `bdwrite`'d. An inode buffer that is released several times in one TXG pays that every time.
- **Cache.** P/Q, zero-fill and mirror buffers are released without `B_RELBUF`/`B_NOCACHE`. Half the newly cached bytes are parity, which is never read, and metadata sits in the cache 4 times. This matters on the Pi's RAM.

**Fix:**
- Set `B_RELBUF | B_NOCACHE` (a one-line change per site).
- `bdwrite` the mirror copies, or queue them for the flush. The R1 ordering already makes them durable before the header.
- Move row sealing to an async per-mount seal path (`bwrite_start` + `biodone`, throttled by `runningbufspace`).

### P4. All metadata reads go to disk 0 [V2]
**Where:** `hammer2_dio_key` (`io.c:143–154`): metadata uses `vol->id`, which is always 0 because `md_extents[0]` lives in volume 0.

**What happens.** Three identical copies sit unused while one USB SSD serves every
lookup. If disk 0 hangs, every metadata lookup stalls.

**Fix:** choose the read mirror per 64 KB block (`(off >> 16) % ndisks`, skipping
failed disks) with a per-disk cache key. Writes stay primary plus mirrors.

### P5. Space and I/O amplification for small and compressed blocks [V2]
- **Whole columns.** `stripe_alloc` gives every chain a whole 64 KB column. A 1 KB file, or a block that LZ4 compressed to 4 KB, still uses 64 KB plus parity, so **compression saves no space on v3**. `newplan.md §5.8` claims linear packing helps; it doesn't, because v3 data bypasses the freemap.
- **Undersized rows.** A row sealed at a flush with one column writes zeros to the other data columns, plus P and Q (4 writes for 64 KB), and the rest of the row is lost until bulkfree.
- **Resize allocates twice** (`chain.c:1396`, `:1406–1412`). `chain_resize` calls `modify` with flags 0, which allocates slot #1, reads and dirties it, and then allocates slot #2. Slot #1 is written and leaked until bulkfree. This happens whenever the compressed size class changes.

**Fix:**
- On v3, make resize update only `bytes` and the radix.
- Longer term: sub-allocate inside a column, and use the column mask (S1).

### P6. Resilver, metadata copy and scrub run at queue depth 1 [V2]
- **Resilver** (`io.c:2325–2420`). Each slot takes an exclusive lock, does N−1 synchronous 64 KB reads, does GF math and a synchronous write, then unlocks. Estimated 15–30 MB/s, so a 240 GB member takes hours, and every seal waits on that lock per slot. A single data erasure also runs a full Q syndrome.
- **Phase A** (`:2115–2270`) copies the *entire* metadata zone (about 5% of each disk, about 12 GB on a 240 GB SSD), used or not, in 64 KB synchronous chunks.
- **Scrub** (`:2540–2760`) is a QD1 walk in tree order and never checks P/Q.

**Fix:**
- Resilver in batches of 16–32 slots with asynchronous reads across survivors.
- Replace the per-slot exclusive lock with a "rebuild window" that writers check.
- Use XOR-only recovery for a single data erasure.
- Persist a resume cursor.
- Copy only the freemap-allocated metadata segments.
- Add a row-ordered parity sweep (S6).

### P7. Degraded reads [V2]
**Where:** `read_degraded_x` (`io.c:1808–1887`).

**What happens.**
- Each call allocates `ndisks × 64 KB` with M_ZERO.
- It reads the siblings serially.
- It always reads Q, and recomputes it, even for a single data erasure.
- Self-heal calls it once per erasure candidate, re-reading the whole row each time (about 9 reads instead of 3 on 4 disks).
- The failed-disk preempt path rebuilds blocks that a NEW is about to zero, and then reconstructs again at `:583`.

**Fix:**
- Issue all sibling reads together, then wait.
- Read only P for a single data erasure.
- Use a pooled scratch set.
- Read the row once and try the erasure combinations in memory.
- Skip reconstruction for NEW.

### P8. Allocator data structures [V2]
- **`stripe_alloc`** (`ondisk.c:2404–2434`) scans one bit at a time and calls `slot_is_data()` (a loop over `md_extents`) per slot, all under `stripe_bitmap_spin`. On a nearly full disk that is O(nslots) per new row: about 3.9M slots at 240 GB.
- **`open_rows` is a TAILQ** scanned linearly by `pack` and `add_data`, and `seal_all` rescans from the head after every seal (O(n²)).
- **Degraded rows never fill.** Rows with a failed data disk never reach `n_alloc == ndata`, so about half the rows of a degraded array stay open until the flush, each holding a 64 KB copy.

**Fix:**
- Precompute a data-slot mask and scan 64-bit words with `ffsl`, keeping per-page free counts.
- Index `open_rows` by `row_id` (hash or RB tree), and keep a separate small "packable" list.
- Have `seal_all` splice the rows off in one pass.
- Treat a row as full once every *writable* data disk is allocated.

### P9. Smaller items
- **Data read-ahead on v3** reads neighbouring slots on the same disk, and half of those are P/Q (`io.c:488–520`). Use `hce = 0` for Path A dios and rely on logical read-ahead. [R]
- **The dio hash drops the disk bits** (`io.c:2830`), so all columns of a row share a bucket. Mix in `pbase >> 56`. [V2]
- **Each data column costs a 64 KB `kmalloc` + `bcopy`** held until the seal (`io.c:779–784`). Large kmallocs cost TLB-shootdown IPIs on free. Use an objcache, or the fold-in accumulation in S3.

---

## 4. Simplifications

- **S-a. One mark-failed and one close-member helper** to replace four mark-failed copies and three close copies. This fixes R3 and R7 together.
- **S-b. Layout inlines.** `slot = (off-SEG64)/unit; p = slot%n; q = (p+1)%n` is open-coded about 5 times, and the "skip P/Q round-robin" picker twice. Add `hammer2_raid6_slot()`, `_pq()`, `_lcol()` and `pick_data_disk()` in `local_hammer2_raid6.h`. A different rotation for 5–6 disks then touches one place.
- **S-c. `hammer2_raid6_v3(hmp)` everywhere.** `raid_type == RAID6 && version >= RAIDZ2` is spelled out at about 15 sites, and `raid_type` is only ever set for v3. Move the misplaced comment at `hammer2.h:2174`.
- **S-d. `stripe_row_refcount` is never decremented.** It is only set to 1, ++ (saturating) or 0, and only tested for ≠ 0, so it is an 8× larger bitmap, including in the persisted space map. Persist a bitmap, or repurpose the byte (S1).
- **S-e. Dead code:**
  - `hammer2_io_raid6_write_scratch` (no callers);
  - the `open_row_pick` prototype;
  - the unreachable failed-disk branch at `io.c:454–473`;
  - the `absent_data` path (if R10/U confirms it is unreachable);
  - unused `DISK_REBUILDING/SPARE/ABSENT` and `FLAG_REBUILDING` constants (either persist REBUILDING during a resilver or drop them);
  - the `raid6_pack_open_rows` bisect knob, whose 0 mode is untested.
- **S-f. Scrub should call the self-heal helper** instead of its own weaker reconstruction (R-minor). That removes one of the three repair implementations.
- **S-g. Dedup on v3.** `hammer2_io_dedup_*` run on every destroyed v3 DATA chain against the wrong (disk-0) dio, harmlessly. Return early on v3. `hammer2_io_data`/`dedup_mask` are correct only because the `(int)` cast drops the disk bits; compute the in-buffer offset explicitly.

---

## 5. Smarter structures and methods

- **S1. Persist a per-row column mask** (a byte per slot, replacing the refcount):
  - which data columns are live, plus "P/Q valid";
  - reconstruction treats non-live columns as zero, so partial-seal parity is exact (R4a) and the zero-fill writes in `row_io` go away;
  - bulkfree can free per column, and the resilver skips dead columns;
  - the mask must be ordered with the P/Q write, like `rz_sm_gen`.
- **S2. Generation-tagged open rows.** Pack only within the current flush generation (see R4). This removes the cross-TXG write hole by construction and makes the flush the natural point to seal everything.
- **S3. Fold parity in as columns arrive.** Keep only P and Q per open row, and fold each column in on arrival (`xor` / `mul(acc=1)`, already NEON). This removes the per-column 64 KB copies. It requires R5 (no rewrite of an arrived column).
- **S4. A per-slot "open" bit** next to `stripe_bitmap`. That gives an O(1) bulkfree guard (R9) and a fast `add_data` lookup.
- **S5. Word-wise bitmap search** over a precomputed data-slot mask (P8). Also usable in `bf_end` and `raid6_space`.
- **S6. Row-ordered online parity scrub.** Read N consecutive allocated rows on all disks in parallel and check P and Q. A row where both disagree localises the single bad column via the syndrome (as in md `raid6check`), and checksums are consulted only for inconsistent rows. This is large sequential I/O instead of a QD1 tree walk.
- **S7. Batched resilver with a moving "rebuild window"** instead of a per-slot exclusive lock (P6).
- **S8. Metadata read spreading** across mirrors (P4).
- **S9. Sub-column allocation** for small and compressed chains (P5), which makes compression worth something on v3.
- **S10. ZFS-style header selection** once R1 is fixed: the newest valid header wins, with no majority needed (R2).

---

## 6. Observability: the §1 bug went unnoticed for months

**What exists today:**
- the upstream logical `iod_*` counters;
- per-disk checksum counters;
- `repair_writes_done/dropped`.

**What is missing:**
- per-disk I/O;
- row seal statistics;
- read-before-write counts.

**Proposed minimal set**, `atomic_add_64` on hmp, exported through a `HAMMER2IOC_RAID_STATS`
ioctl and printed by `hammer2 raid stats`:

- per disk: `rd_ops/rd_bytes/wr_ops/wr_bytes`, with writes split by class: data, P, Q, zero, meta_mirror, volhdr;
- `seal_full`, `seal_partial`, `seal_reseal`, `seal_idle_warn`;
- `new_preread` (any read under NEW/NEWNZ; must stay 0);
- `degraded_read_1/_2`, `selfheal_queued`;
- `open_rows_cur/_high` (`open_rows_high` is already collected but never exported);
- `resilver_slots_copied/_skipped`.

---

## 7. Test gaps (cheap tests first)

1. **Group P, "perf shape"** (about 30 s). On a fresh array, `dd` 256 MB plus `sync`, then assert:
   - array reads ≈ 0;
   - per-disk write bytes within ±10% of each other;
   - total writes ≤ 2.1× user data;
   - `new_preread == 0`.

   Repeat with 10k × 4 KB files, which would catch P1 today. The `/var/tmp/dsrw` devstat
   tool used for §1 (libdevstat, 25 lines) can go into `src/diag/`.
2. **Repro tests for R5** (sync rewrite after a row seal) **and R6** (replace with a member disk; must fail).
3. **Split-header crash** for R2: write the newest header to only 1 or 2 of 4 disks (dd an older copy over the others), then mount with and without `j2_allow_rollback`. Today `crash_host.sh` resets the whole VM, so all disks always see the same cut.
4. **Write-side fault injection.** Existing EIO injection covers reads only. Add:
   - `inject_wio_disk_mask` (EIO on write);
   - `inject_delay_ms_disk_mask` (hang without failure).

   Use them in a group that writes during a hang and asserts no panic and an auto-failed member. Add a QMP `device_del` test for `disk_gone`. Today these paths are exercised only by the Pi's USB disks.
5. **Lifecycle tests:**
   - unmount and `umount -f` during a resilver or scrub;
   - fill to ENOSPC, then delete + bulkfree + write;
   - `NDISKS=6` in a periodic matrix;
   - `raid6_simd=0` vs `1` parity cross-check on arm64.

---

## 8. Docs and hygiene

- **`hammer2_raid6.patch` lags `src/`**: it lacks `035b1b7` and today's fix. `install_raid6.sh` uses the patch, `deploy.sh` uses the overlay, and no script regenerates the patch. Add `bin/mkpatch`, and have `deploy.sh` warn when `src/` is newer than the patch.
- **`docs/outstanding.md`** is from May (44/44, "I/O timeouts not implemented", "blocked on hardware"). Rewrite it as the current gap list, which this file can seed.
- **`DEVELOPER.md`** has several stale claims:
  - DIRENT is listed as mirrored; it is striped.
  - P/Q writes are described as synchronous `bwrite`; they are parallel `bwrite_start`/`wait`.
  - `seal_all` is said to seal every row; partial seals remain open.
  - It names `test_raid6.c`; the file is `h2raid6_test.c`, and `deploy.sh` doesn't build `h2stripe_check`, so F2 fails on a fresh guest.
  - The test table stops at K.
- **`newplan.md §5.4/§5.6`** still describe one slot per write and "no P/Q ever overwritten". Packing and re-sealing changed both; see R4. `io.c:445` cites §5.4 as the reason no pre-read is needed.
- **`tests/v3`:**
  - `run_all.sh:79` omits N from the usage text;
  - temp files are still named `v4_*`;
  - the K2 comment cites a nonexistent "open_rows cap = 16".
- **Remove:**
  - `hammer2_raid6_patch.sh`: a February `sed -i '124,150c…'` hotfix that would corrupt today's file;
  - `fresh_disks.sh` (vn images, unsupported);
  - `tests/other/` (17 of 18 scripts use vnconfig);
  - the obsolete v1/v2 tools in `src/diag` (`h2parity_fix`, `h2diag*`, `diag_map`);
  - `hammer2_raid6.patch.backup`;
  - the 3 MB session JSON.

  Also fix the "format version 4" string in `h2stripe_check.c`.

---

## 9. Suggested order of work

1. **Commit §1, and P1 (sub-64 KB NEW).** Small and same-family. Add group P with the devstat tool so this class can't come back.
2. **Write repro tests R5, R6 and split-header R2.** Each is a few lines; they turn [R] into facts.
3. **Correctness core, in one series:**
   - R1 three-phase flush (also P2);
   - R3 single mark-failed helper, persisted;
   - R2 quorum votes all zones, allows one-step rollback by default, fails lagging disks;
   - R6 replace validation;
   - R7 close-member helper.
4. **R4 and R5 via generation-tagged rows (S2),** then decide between flush-waits-for-columns and the column mask (S1). This is the biggest design change; write it up in `newplan.md` first.
5. **R8, R9, R10.** Mostly local fixes once the helpers exist.
6. **Performance on the Pi**, measured with the new stats:
   - P3 (`B_NOCACHE` first, it's free);
   - P4;
   - P6 resilver batching;
   - P8;
   - P5/S9 sub-column allocation last.
7. **Cleanup:** S-a to S-g and §8, in any order.
