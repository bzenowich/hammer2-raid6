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
  - **[V-measured]** Confirmed by a measurement on the hardware.
  - **[V2]** Two reviewers reported it independently and the code quotes match, but I did not re-check it myself.
  - **[R]** A reviewer traced it, but nobody has reproduced it.
  - **[U]** Unverified trigger or reachability.
- No finding except §1 and P9 (read amplification) has been reproduced on h2dev or the Pi yet. §7 lists the cheap repro tests to write first; §0 explains why the suite missed these bugs.
- Line numbers are from `src/sys/` at the tree above.

---

## 0. Why the test suite missed these bugs

`tests/v3` copies the *scenarios* of the mdadm test suite: fail a disk, two failed
disks, replace and resilver, scrub, degraded mount, and unclean shutdown. It does not
copy md's *checks* or its *kinds of failure*. Almost every test drives a feature and
then asks one question: do the file contents read back? Most bugs in §2 cannot
change that answer.

### 0.1 The crash test cannot lose a write
- `crash_host.sh` "cuts power" with QMP `system_reset`.
- `qemu-machine.sh:75` attaches the RAID disks with `cache=writeback`.
- So every write QEMU has acknowledged is still in the host page cache, and is still there after the reset.
- The test simulates "stop sending I/O", not "lose the drive's volatile write cache".
- All four disks also stop at the same instant.

R1 (header written before the other disks flushed) and R2 (split headers) only show up
when unflushed writes are lost, or when the disks are cut at different points.
This test can produce neither.

In the md world this testing is done by separate tools (`dm-log-writes`, `dm-flakey`,
the xfstests generic/log-writes tests), not by the mdadm suite. Copying the mdadm
suite brought in its blind spot.

### 0.2 md's main correctness check was lost
- After most operations, the md tests run a `check` pass and require `mismatch_cnt == 0`.
- Our equivalent, `h2stripe_check`, runs once, in F2 (`test_f_cow_invariant.sh:54`).
- Every other test checks only file contents.

RAID6 is built to hide bad parity: block checksums plus self-heal return correct data
whether or not parity is right. Bad parity only shows when a second disk fails. These
bugs all leave reads correct and parity wrong until a second failure:
- R4: partial-seal garbage column;
- R2c: a lagging disk trusted;
- R3: a disk failed in memory only;
- R10c: a repair onto a reallocated slot.

### 0.3 Failures only happen at tidy moments, and only on reads
- Disks are failed in three ways:
  - by ioctl (`fail-disk`);
  - by being left out of the mount spec;
  - with errors on reads.
- `hammer2_inject_eio_disk_mask` covers only `bread`/`breadnx` (`local_hammer2.h:2281–2296`).
- Nothing makes a write or a cache flush fail.
- Nothing hangs a disk without failing it.
- Nothing fails a disk partway through a write, a seal or the header loop.

Those are exactly the weak paths in R3, R7 and R11. They were exercised only by the
Pi's real USB dropouts. That is why the Pi found bugs that h2dev never did.

### 0.4 Our design has failure modes md doesn't
md rewrites stripes in place, under a stripe cache and a write-intent bitmap. v3 has
different machinery:
- copy-on-write;
- rows that fill across transactions;
- deferred seals, and partial seals at a flush;
- a header quorum.

Copying md's scenarios covered md's risks, not the ones this design adds. No test
rewrites a block in the same TXG after its row sealed (R5), because md has no such state.

### 0.5 Correct output is not correct behaviour
- **Performance.** The §1 bug produced correct output, so no functional test could catch it. Only an I/O-count check could.
- **Admin mistakes.** md is protected from the wrong-replace-disk mistake (R6) by the Linux kernel's exclusive open of member devices, so the mdadm suite never needed that test. On DragonFly we have to check it ourselves.

### 0.6 The tests are sequential and small, and coverage is not measured
- The tests are shell scripts that run one step after another on 4 GB virtual disks.
- So they hit the narrow timing windows only by luck: the remaining vinvalbuf paths (R7), and a rewrite after a seal (R5).
- Nothing records which kernel paths a test actually reached. The 2026-10-04 audit found that the old 65/0 pass was largely vacuous for this reason.

Most findings in §2 are still [R] or [U], so some may turn out not to be real. Closing
these gaps is how to find out.

### 0.7 What would close the gaps, in order of value
1. **Parity check after every group.** Run `h2stripe_check` on the unmounted array at
   the end of each group. Add a check that every disk's newest header has the same
   sequence and agrees with the others. This is cheap, and restores md's main check.
2. **A crash test that can lose writes.**
   - Use `cache=none` or `directsync`.
   - Add a recording layer, as a host-side QEMU block filter or a guest shim under each member, that logs every write and flush per disk.
   - Replay up to every flush point, including combinations where each disk is cut at a different point.
   - After each replay: mount, run the quorum, then the parity check.

   This is the `dm-log-writes` method, and it tests R1 and R2 directly.
3. **Error and hang injection for writes and flushes.** Add `inject_wio_disk_mask`,
   `inject_flush_err_disk_mask` and `inject_delay_ms_disk_mask`. Add a QMP `device_del`
   of a member while it is being written, to exercise `disk_gone`.
4. **I/O-count checks (group P, §7):** no reads during writes, even per-disk writes, and
   total writes at most 2.1× the user data. A read-side check belongs here too: a cold
   sequential read on the Pi currently moves about 2.2× the file data, because
   read-ahead pulls in P/Q (P9).
5. **A random concurrent stress run:** fsx or fsstress, with a `fail-disk`, `replace`
   or `scrub` partway through, ending with the check in item 1.
6. **Counters that prove each path ran** (§6). Every test asserts that the counters for
   its target path went up: `seal_partial`, `degraded_read_2`, `selfheal_queued`,
   `new_preread == 0`. A test then can't pass without exercising its code.

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

**Design: §10** (checked against the code; one claim corrected, zero-fill is unsafe as written).

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

**Repro status (r5-repro, 2026-10-11): REPRODUCED, panic on h2dev.** `tests/v3/test_r_rewrite_seal.sh` (group R, opt-in: `sh run_all.sh R`, not in the default list).
- **Recipe** (fresh 4-disk array, no `sync` in between): O_SYNC `pwrite` of 4 KB to file A; eight more 4 KB O_SYNC files so A's row fills and seals; O_SYNC `pwrite` of 4 KB to A again.
- **Result:** `panic: hammer2: v3 putblk for a row not open: phys 000000000dc00000 disk 2`, stack `hammer2_raid6_open_row_add_data` <- `_hammer2_io_putblk+0x53d` <- `hammer2_xop_strategy_write+0xca8` <- `hammer2_primary_xops_thread`. Seen three times, identical.
- **Why the path is open (confirmed from the code):**
  - `add_data` runs at the last drop of a dirty DATA dio, i.e. at every strategy write (`hammer2_xop_strategy_write` -> `hammer2_write_file_core` -> `hammer2_io_bawrite/bwrite`), not at chain flush. The same chain's data can be written any number of times per TXG.
  - A strategy write happens whenever the logical buffer is written out: O_SYNC `bwrite`, `fsync`, the syncer, write-behind.
  - `hammer2_dedup_record()` returns early on v3, so `HAMMER2_CHAIN_DEDUPABLE` is never set, and the `DEDUPABLE -> newmod = 1` branch in `chain_modify` (which would otherwise save this case) is dead on v3.
  - So a still-MODIFIED data chain keeps its slot (`newmod = 0`); a full row seals and leaves `open_rows` as soon as its last column arrives; the second putblk finds no row and panics. The stale comment above the "latest copy wins" code in `add_data` describes a case that only works while the row is still open.
- **A plain `fsync` of A does NOT trigger it** (control R3 passes): `hammer2_vop_fsync` -> `hammer2_inode_chain_flush` flushes the file's data chains and clears MODIFIED, so the next modify takes the fresh-slot path (`newmod = 1`). The trigger needs a data rewrite with no flush of that chain in between (O_SYNC writes, or two bufcache flushes before the syncer runs).
- Not tested (would need the panic fixed first): the data/parity corruption variant when the row is still open but already partially sealed (R4).

**Design: §10.2.**

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
- **Data read-ahead on v3** reads neighbouring slots on the same disk, and half of those are P/Q (`io.c:488–520`). Use `hce = 0` for Path A dios and rely on logical read-ahead. Measured on the Pi on 2026-10-10 (kernel #16): a cold sequential read of 1 GB read about 170 MB/s across the four disks to deliver 78 MB/s of file data, about 2.2× read amplification. Fixing this should roughly double sequential reads. [V-measured]
- **The dio hash drops the disk bits** (`io.c:2830`), so all columns of a row share a bucket. Mix in `pbase >> 56`. [V2]
- **Each data column costs a 64 KB `kmalloc` + `bcopy`** held until the seal (`io.c:779–784`). Large kmallocs cost TLB-shootdown IPIs on free. Use an objcache, or the fold-in accumulation in S3.

### P10. Small-file write amplification: every dirty metadata release mirrors 3 x 64 KB synchronously [V-measured]
**Where:** `_hammer2_io_putblk`, `local_hammer2_io.c:932–948` (the `rz_meta` branch) calling
`hammer2_io_metadata_mirror_write` (`local_hammer2_raid6.c:705`). This is the cost P3 names
("an inode buffer that is released several times in one TXG pays that every time"), now measured.

**Symptom.** Group P2 (`cp -R` of 4000 x 4 KB files = 15.6 MB, then `sync; sync`) writes
5.1 GB to the members, 326x the user bytes, identically on h2dev and the Pi. Disk 0 gets 142 MB,
disks 1..3 get 1669 MB each.

**Method.** Temporary counters in `putblk` (not committed): dirty metadata releases per btype,
and distinct 64 KB offsets among them; devstat per member. Runs at 100, 500, 1000 and 4000 files
on h2dev, fresh array each time.

| files | written to members | per file | disk 0 | disks 1..3 each |
|------:|-------------------:|---------:|-------:|----------------:|
| 100   | 132 MB  | 1354 KB | 3.6 MB   | 42.9 MB |
| 500   | 590 MB  | 1207 KB | 16.6 MB  | 191 MB  |
| 1000  | 1273 MB | 1304 KB | 33.9 MB  | 413 MB  |
| 4000  | 5148 MB | 1318 KB | 142 MB   | 1669 MB |

Linear in the file count, so it is a per-file cost and not a flush-pass cost.

**Byte accounting at 4000 files** (counters read after the sync):

| source | dirty releases (64 KB mirrored 3x) | distinct 64 KB blocks | bytes to members |
|---|---:|---:|---:|
| INODE metadata blocks | 16080 (4.0 per file) | 63 | 3015 MB |
| INDIRECT blocks (directory tree) | 4383 | 53 | 822 MB |
| FREEMAP_LEAF | 4227 | 3 | 793 MB |
| FREEMAP_NODE | 0 | 0 | 0 |
| **metadata mirror total** | **24690** | **119** | **~4630 MB (counters), 4578 MB (devstat: disks 1..3 minus disk 0's share)** |
| DATA rows: 4000 columns packed 2 per row, so 2000 rows of 2 data + P + Q | | | ~512 MB, 128 MB per disk |
| primary metadata (`bdwrite`, on the dio's own disk 0) | | 119 | ~8 MB |
| space map, volume headers, rest | | | ~50 MB |

The same 119 metadata blocks (64 inode blocks hold the 4000 inodes, about 63 per block) are
written 24690 times: **a factor of 200 redundancy** in the mirror writes. DATA/DIRENT and parity
are fine (512 MB for 16 MB of user data is the known 64 KB-column cost, P5, 32x, and
is balanced).

**Why disk 0 is different.** The dio's own disk (always disk 0 for metadata, P4) gets the
block through `bdwrite`, which the buffer cache coalesces: a block released 135 times is written
to disk 0 about once. The N-1 mirror copies bypass the cache coalescing: `mirror_write` does a
`getblk` + `bcopy` + `hammer2_bwrite_start`/`wait` on every release, and the buffer is written
out for real each time, so disks 1..3 take 24690 x 64 KB each = 1.5 GB. That accounts for the
whole skew: disk 0 = its share of data and parity (128 MB) + 8 MB of coalesced metadata.

**Why a release is not once per TXG.** Chain locks are cycled constantly while files are created
(inode, parent directory block, freemap leaf each get unlocked/released after every operation),
and `putblk` runs for the 1->0 dio ref transition. Each dirty release is a complete 64 KB
write times 3 mirrors, though the inode has only changed by a few hundred bytes.

**Fix (no on-disk format change, row sealing untouched).** Defer the mirror copies to the flush:
`putblk` queues the block's bytes (the buffer it already kmalloc'd) in a per-mount table keyed by
the per-disk offset, replacing any earlier copy; `hammer2_vfs_sync` writes every pending mirror
once, after the chain flush and before the `VOP_FSYNC` drain and the volume header, so a mirror
is exactly as durable at the header commit as before (R1 ordering unchanged). The table is also
searched by `metadata_mirror_read` so a failover read never sees a stale mirror, a bound on the
pending count forces an early flush, and the degraded "primary dropped" release still mirrors
immediately. Expected: 24690 mirror block-writes become 119 x 3 and total writes fall from
5.1 GB to roughly 0.7 GB (about 45x user bytes, the rest being P5's whole-column cost).

**Fixed (fix/p2-writeamp).** `putblk` queues the copy (`hammer2_io_metadata_mirror_defer`),
`hammer2_vfs_sync` writes it once (`hammer2_io_metadata_mirror_flush`, before the VOP_FSYNC
drain), unmount flushes the rest, and `metadata_mirror_read` serves queued copies. Group P2 on h2dev:
5148 MB (326x) becomes 536-582 MB (34-37x), per-disk deviation 4%, array reads 128 KB. The
remainder is the 512 MB of whole-column data and parity (P5). Group P asserts balance and
a 40x limit for P2. An R1 pass count does not change this (a clean run with P1+R1+R2 still wrote 328x).

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
6. **Seen once, not reproduced (2026-10-11):** page fault in `hammer2_characterize_failed_chain+0xd6` (deref of `chain->dio->bp`) from `hammer2_chain_testcheck`, during group B on h2dev, a degraded 3-disk mount with a concurrent read-path self-heal repair write. The function is marked "not really MPSAFE". vmcore.9 had no module symbols, so the line was not confirmed. Group B passed 13/13 in the two reruns. Suspect: the repair path releases or replaces `chain->dio` while the CHECK FAIL report reads it. Build the module with debug info for h2dev runs so the next dump is usable.
7. **Seen during R8 work (2026-10-11), not fixed:** when a mount is refused after the super-root fails its check, the unwind can panic in `hammer2_pfsfree_scan` → `hammer2_inode_drop` ("page fault ... panic with 1 spinlocks held"). It was triggered by L4 before the `TESTEDGOOD` fix in ccdda50, and probably affects any mount refused at that stage. Repro: corrupt the super-root on every copy and mount; the mount must fail with an error, not a panic.

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

---

## 10. R4/R5 design: generation-tagged rows

Design only, written against `824811e` (R1 three-phase flush merged). Nothing here is
implemented. It decides §9 step 4.

### 10.1 What the code does today (checked)

- **R4 confirmed.** `row_io` skips reserved columns (`alloc_mask` test), so a
  partial seal leaves the slot's old bytes under P/Q computed as zero.
  - (b) confirmed: a late column makes `add_data` seal the `partial` row again, so P/Q are rewritten in place over committed parity.
  - (c) confirmed: `add_data` skips the seal while `sealing` is set, and `seal_all` never revisits a row in the same pass (`seal_gen`). The column and a header that commits it can land with the old P/Q.
- **The review's premise for S2 is only half true.** Rows do not normally span TXGs:
  `seal_all` seals every complete row and removes it, so after a flush `open_rows` holds
  only `partial` rows, and those are already closed to packing. The real hazard is the
  *flush window*: `seal_all` runs after the vchain flush, and a chain allocated in that
  window packs into a row that is then partial-sealed. S2 therefore means "put a cut at
  the right place and wait", not "stop packing across TXGs".
- **The reservation window is short.** `stripe_alloc` runs in `assign_physical` (via
  `chain_modify`/`chain_create`) and the put happens at the last drop of the dio in
  `write_bp`, in one `hammer2_xop_strategy_write` call, with the chain locked. A column is
  pending only for the length of that call, unless the reservation leaks (next point).
- **Leaked reservations exist today** (this is R9's suspected source; confirmed by reading).
  `hammer2_chain_resize` calls `chain_modify(.., flags 0)`, which allocates a slot for an
  unmodified chain, and then calls `stripe_alloc` again. The first slot is never put. It is
  hit whenever `assign_physical` sees `chain->bytes != pblksize`, i.e. a small file growing.
  Such a row never completes and is partial-sealed at every flush ("still waiting" log). Any
  design that *waits* for pending columns must fix this first.
- **Zero-fill at partial seal is unsafe as written** (new finding). The zero write in `row_io`
  is `getblk(devvp, phys_off)` + `bzero` + `bwrite`, the same buffer key as the column's
  `bdwrite` in `putblk`. If the column arrives while the seal I/O runs unlocked, the zero
  write destroys real data. It is only safe once the reservation is cancelled under the spin.
- **`mirror_tid` is the wrong counter.** It is bumped inside `hammer2_flush` (flush_core,
  vchain/fchain), before the point where `seal_all` runs.
- **Flush is not a TXG cut.** `hammer2_flush` visits whatever is MODIFIED when it gets there,
  and frontend writers are not blocked (normal transactions do not interlock with ISFLUSH).
  So the header can commit a blockref whose chain was allocated a moment earlier. Every chain
  the header commits was allocated before the vchain flush returned.
- Only the syncer (`vfsops.c:3324`) passes `XOP_VOLHDR`; `fsync(2)` never commits a header or
  runs `seal_all`. The new wait therefore costs fsync nothing.

### 10.2 Design

**Generation.** `hmp->rz_gen` (uint32, under `stripe_bitmap_spin`; reuse the field
`open_rows_sealgen`, whose only use is the visit marker). Not `rz_sm_gen`/`stripe_generation`
(space-map write counter) and not `mirror_tid`. A new `r->gen = hmp->rz_gen` is set in
`open_row_register_locked`.

**Where it advances.** At the top of the seal phase in `hammer2_xop_inode_flush`: after the
vchain flush returns and all chain locks are dropped, before the mirror drain, the device
fsync and R1 phase 1. That is the latest point where "everything the header can commit is in a
row of gen <= G" still holds, and the earliest point that does not sweep later allocations
into this flush. Advancing earlier (before `hammer2_flush`) would let a header commit a chain
whose row is gen G+1 and unsealed, which is the R1 hole again.

**Allocation.** `open_row_pack_locked` skips rows with `r->gen != hmp->rz_gen`. New rows get the
current gen. Rows of gen <= G are closed to new reservations the moment the gen advances, so
their set of pending columns can only shrink.

**Flush.** `hammer2_raid6_seal_all_open_rows` is replaced by `seal_gen`:
1. under the spin, `G = rz_gen++`;
2. for every row with `gen <= G`: if `pending_mask == 0` (all reserved columns arrived), unlink and seal it as today (`seal_row_locked_to_unlocked`, full, once);
3. otherwise `ssleep(&hmp->open_rows, &spin, 0, "h2rzgen", hz/10)` until the column arrives (`add_data` does `wakeup` when it completes a row of an old gen and a waiter is flagged);
4. after `vfs.hammer2.raid6_gen_wait_ms` (default 5000) of no progress, **fail the flush**: set `fsync_error`, so the header is not written (the existing `fsync_error == 0` guard), log the row, leave it on the list. The next flush waits again (its gen is still old).

Rows of gen G+1 are untouched by this flush, so no row is ever sealed with a pending column:
`partial`, `sealing`, `dirty`, `idle` and `seal_partial_locked` are deleted. R4 (a), (b), (c) all
disappear. `add_data` still seals a *full* row (`n_alloc >= ndata`) itself, as today.

**Crash consistency (against R1).** The header for T is written only after: every gen <= G row
has all its columns and its P/Q written (`row_io` waits on P/Q), the mirror drain, `VOP_FSYNC`
and the phase-1 cache flush. A crash before the header leaves T-1 committed; the gen <= G slots
are free in the committed space map (written after the seal, from the same flush) and are
overwritten in full when reallocated. Gen G+1 rows are written concurrently but nothing
committed references them. A crash after the header sees exact parity on every committed row.
No committed row is ever re-sealed in a later TXG.

**Failed flush on timeout** trades the old silent-bad-parity for a visible error, and keeps
committed state valid. The zero-fill and S1 options (10.4) would let it commit instead.

### 10.3 R5: same-TXG rewrite

**Chosen: a rewrite after the column has reached its row takes a fresh slot (newmod=1).**
Rule, in `chain_modify`'s "already MODIFIED" branch, for v3 DATA/DIRENT:
`newmod = hammer2_raid6_col_arrived(hmp, bref)`, i.e. 1 unless the chain's column is still
pending in an open row. The tracker answers under `stripe_bitmap_spin`: no open row for
`phys_off` (S4 bit clear, 10.5) means sealed, so 1; a row with `col_data[d] != NULL` means
arrived, so 1; `col_data[d] == NULL` means pending (a deferred first put), so 0 and the put
coalesces into it. A pending column pins its row open, so the check cannot race a seal.
`stripe_alloc` is the existing path and the new slot goes to the current-gen row.

Why not the alternative (keep the gen's sealed rows until flush, recompute P/Q):
- memory: it retains every column of a TXG (up to 30 s of writes, GBs on the Pi) unless it early-seals, and early seal reintroduces the rewrite-after-seal case it was meant to avoid;
- it needs a "rewrite in flight" pin so the flush cannot seal under a rewrite, with a release on every error path between `chain_modify` and the put;
- it blocks S3 (fold on arrival, drop the copy). The chosen rule guarantees an arrived column is never replaced, which is exactly S3's precondition.

Cost: a rewrite burns one 64 KB column and its share of P/Q until two bulkfree passes. This is
the same price `fsync` already pays today (a flushed chain clears MODIFIED, so the next write
takes a fresh slot). Worst case is an O_SYNC loop on one block: 64 KB of slot per iteration.
No rate limiter in the first cut; add `vfs.hammer2.raid6_rewrite_slots` (counter) and decide
from numbers. `add_data`'s "latest copy wins" stays only for the pending case; a second put on
an arrived column becomes a counted anomaly.

### 10.4 R4: which option, and the others later

| | flush waits (chosen) | zero-fill at partial seal | S1 column mask |
|---|---|---|---|
| partial seals | none | still happen | still happen, exact |
| on-disk format | none | none | space map v1 -> v2 |
| fixes (a) | yes | yes | yes |
| fixes (b) | yes (no re-seal) | no, needs gen rows too | yes |
| flush latency | + in-flight writers (ms) | none | none |
| extra I/O | none | zero writes for pending cols | none |
| risk | stuck reservation fails the flush | clobbers an arriving column (10.1) | mask must be ordered with P/Q (like `rz_sm_gen`) |

- **Now: flush waits** (needs 10.2 and the resize leak fix). No format change, deletes more code than it adds.
- **Later, zero-fill:** only as the timeout fallback, and only after the reservation is cancelled under the spin (`add_data` to a cancelled column must drop the data before the `bdwrite`, which means claiming the row before the buffer is disposed in `putblk`). Not worth it until a real stuck reservation shows up.
- **Later, S1:** worth it with S9 (sub-column packing) and per-column free. The persisted map changes: today one byte per slot (a live-chain refcount, `HAMMER2_SM_PAGE_SLOTS == HAMMER2_PBUFSIZE`). A column mask needs up to 32 bits (`alloc_mask` is uint32, `HAMMER2_MAX_VOLUMES` is 64) plus a P/Q-valid bit, so either 4 bytes per slot or an 8-disk limit. Bump `HAMMER2_SM_VERSION` to 2; `sm_read` already treats a mismatching header as "map invalid" and rebuilds by a tree walk, so old arrays mount and convert.

### 10.5 R9 and S4

The per-slot open bit falls out and is cheap: `hmp->stripe_open` (one bit per slot, same size as
`stripe_bitmap`), set in `open_row_register_locked`, cleared where a row is unlinked, both under
the spin. Uses: (1) `bf_end` skips (and does not stage) a slot with the bit set, closing R9;
(2) `col_arrived` and `add_data` answer "no row" in O(1). The resize fix is the other half:
`hammer2_raid6_stripe_release(hmp, bref)` clears the reservation (alloc_mask bit, n_alloc,
refcount; frees an empty row) when the column was never put.

### 10.6 Concurrency

- **Lock.** `stripe_bitmap_spin` only, as today. Never held across kmalloc/kfree/I/O (the existing seal already drops it around `row_io`). `ssleep` releases it.
- **Callers.** XOP strategy threads (`stripe_alloc` via `chain_modify`/`chain_resize`/`chain_create`, `add_data` via `putblk`); the flusher, which is the XOP thread running `xop_inode_flush` (`seal_gen`); bulkfree (`bf_end`); the syncer thread only waits on the flush XOP.
- **With chain locks.** `col_arrived` and `stripe_release` are called with the chain locked and take only the leaf spin, the same as `stripe_alloc` today. The flusher holds no chain lock, no dio ref and not `rebuild_lk` while it waits (all dropped before the seal phase; `rebuild_lk` is first taken in the header section). A writer whose put the flusher waits for needs only chain locks the flusher has released.
- **Checked (phase 2).** Strategy writes run on XOP groups `0..sgroups*mod`; non-strategy XOPs, including `xop_inode_flush`, use `xbase` and above, so the two sets of threads are disjoint and a flush XOP never queues behind a pending-column write. `hammer2_trans_init(ISFLUSH)` interlocks only against other ISFLUSH transactions; strategy writes run in normal/BUFCACHE transactions and never block on the flush. The flusher holds no `rebuild_lk` across the wait. Only the syncer's VOLHDR flush runs `seal_all`; `fsync(2)` does not wait.
- The timeout turns any missed deadlock into a failed flush, not a hang.

- **Liveness (reviewed).** Every reservation reaches its put: stripe_alloc registers only on success; the strategy XOP runs alloc, newnz (sets DIRTY in every branch) and the drop in one pass; `hammer2_io_inval` is a NOP so a deleted chain's dio still writes at lastdrop; a logical buffer discarded by rm/truncate never reaches strategy, so it never reserves; v3 has dedup off. Gaps found and closed with `hammer2_raid6_col_release` (zero column, dropped, counter `rz_col_released`): a DIRTY DATA/DIRENT dio that lastdrops without GOOD (io error in getblk, failed degraded rebuild), and `chain_modify`'s bread error after the alloc. Untested by the suite (needs 3 failures or injection).

### 10.7 Tests

- **Group R (R5) must pass**: the three-write recipe, then umount and `h2stripe_check` clean.
- **New R4(a), deterministic.** Needs a test hook: `vfs.hammer2.raid6_put_stall_ms` (RW, 0 default), a sleep in `write_bp` between the alloc and the dio drop for one write.
  1. fresh array, fill with random files, `rm`, `bulkfree` x2 (free slots now hold garbage);
  2. stall 3000, start one 4 KB write in the background, `sync` in the foreground;
  3. assert `sync` took at least ~3 s (it waited) and that the header was written;
  4. host `system_reset` right after `sync` returns (as `crash_host.sh`), after the guest boots and before mounting, `h2stripe_check` on the raw disks (it needs the array unmounted): every committed row has exact parity.
  - Before the fix the same run shows a parity error on the garbage column. Also a timeout variant: stall > `gen_wait_ms`, expect `sync` to report an error and the committed state to be T-1 (`h2stripe_check` clean).
- **Leak test (J):** grow a small file 4 KB -> 8 KB -> 16 KB, `sync`, expect `open_rows_count == 0` (expose count/high as a sysctl).
- **Regression groups:** F (COW invariant, parity), J (packed rows, bulkfree with survivors), D (resilver reads rows), K (scrub, writes during scrub), N (capacity, bulkfree, reuse), P (I/O shape), I and `crash_host.sh` once. T (reuse) and A as usual.
- **Expected perf.** Group P amplification (45x limit) unchanged: normal writes still take the same path. Flush latency: + the time of in-flight strategy writes (ms; the XOP threads are idle at a tidy flush). One P/Q write per row instead of two for rows that were partial-sealed. O_SYNC rewrites cost one extra column each (10.3). P2 (small-file write) is untouched.

### 10.8 Implementation plan (small commits)

1. **Resize radix (was: R9 leak):** the resize leak claim in 10.1 was wrong (the old S_a copy is put; `open_left` is 0 on J and N). The real bug was `chain->bytes` still the old size when `stripe_alloc` ran, giving a radix/lsize mismatch panic in `_hammer2_io_getblk`. `stripe_release` was not needed. Stats sysctls for open row count/high. Test: J3.
2. **S4 + R9 guard:** `stripe_open` bitmap, set/clear, `bf_end` skip. Test: N, J.
3. **R5:** `col_arrived`, `chain_modify` rule, `add_data` anomaly counter. Test: group R now passes; F.
4. **S2:** `rz_gen`, `r->gen`, pack filter, `seal_gen` with wait and failed flush, delete partial-seal state; `flush.c` handles the error. Test: J, F, D, K, I.
5. **Test hooks and R4(a) test:** `raid6_put_stall_ms`, `raid6_gen_wait_ms`, the new tests and the `crash_host.sh` variant.
6. **Later:** S3 fold-in (R5 is its precondition), P3 async sealing, then re-evaluate S1 with S9.

### 10.9 Open questions (answered by the coordinator; implemented as noted)

- Timeout: the flush fails (EAGAIN, `rz_gen_wait_fail`, one kprintf with the stuck row); unmount never waits and commits T-1 with a kprintf. Cancel + zero-fill only if the counter is ever nonzero in soak.
- `add_data` keeps the panic for a missing row; `rz_put_anomaly` counts a second put.
- `raid6_gen_wait_ms` is RW, default 5000.

Original questions:

- Timeout policy: fail the flush forever on a permanent stuck reservation (unmount then cannot commit), or after N failures cancel the reservation and zero-fill? The second needs the claim-before-bdwrite reorder in `putblk`.
- `add_data` on a missing row is a `panic` today. Keep it through soak (it found R5), or log and drop?
- Is 5 s the right default wait on the Pi with a USB disk stalled in the middle of a write?

## 11. P5/S9 design: sub-column allocation

Design only, written against `e098809` plus the P5 resize fix (below). Nothing in 11.2 on is
implemented. It builds on §10 (generation-tagged rows, R5 `col_arrived`) and decides the "last" item
of §9. File:line references are to this tree.

### 11.1 Goal and numbers

- **P2 today:** 4000 x 4 KB files (16.4 MB) write 536-582 MB to the members, 34-37x. 512 MB of that
  is data: every chain takes a whole 64 KB column, 2000 rows x (2 data + P + Q) x 64 KB. The rest is
  metadata (P10). Space: each 4 KB file reserves 64 KB of data column, **16x** the user bytes, 24x
  with parity.
- **Compression saves nothing on v3.** A block LZ4-compressed to 4 KB still takes a column
  (`stripe_alloc` at `ondisk.c:2738` takes a whole slot or column per chain).
- **Targets (P2, 6-disk 4+2):** data and parity 16.4 MB x 1.5 = ~25 MB (16 chains per column, 64
  per row), plus the ~25-70 MB metadata of P10 gives **60-95 MB, 4-6x; assert <= 8x** (was 45x).
  Space: 4000 files reserve ~1.5-2x user bytes (was 24x). 4000 compressible 4 KB files (1 KB after
  LZ4) must use <= 1/3 of the columns of the incompressible ones.
- A big sequential write (P1, 64 KB blocks) must not change.

### 11.2 Facts checked in the code

- **Part A (merged first, same branch): resize.** The old `chain_resize` called `chain_modify`
  with flags 0 (reads and copies the old block into slot #1, which is put and leaks) and then
  `stripe_alloc` again (slot #2). J3 counted 200 columns for 120 chain writes. Now one
  `chain_modify(OPTDATA)` after `chain->bytes` is set (`chain.c:1373` `hammer2_chain_resize`): a
  pending column is kept and only the radix of `bref.data_off` changes (counter `rz_resize_inplace`);
  an arrived one or an unmodified chain takes one fresh slot through the §10.3 rule. J3 now 120.
  P2 never resizes, so it is unchanged. The review's "slot #1 leaks until bulkfree" is right, but
  §10.1 was right that it is put, not a pending reservation.
- **The dio is always 64 KB and the offset is free.** `psize = HAMMER2_PBUFSIZE`, the dio key is
  `pbase`, and `lbase` need only fit inside one pbuf (`io.c:185-203`); `hammer2_io_data(dio, lbase)`
  addresses inside it (`io.c:1127`). `data_off` = `lbase | radix`, radix in bits 0-5, so bits 6-15
  can carry a 1 KB-unit offset (`HAMMER2_ALLOC_MIN` 1024, `disk.h:86`). **No blockref format change.**
- **`bref_slot` already ignores the sub-offset** (`ondisk.c:1487`: `phys_off / stripe_unit`), so the
  slot, bulkfree marking and the space map all stay per row.
- **Check codes are per chain, over `[off, off+bytes)` of the column**:
  `hammer2_bref_check_match(bref, copy + off, bytes)` in `metadata_mirror_heal` (`io.c:1967`);
  `scrub_verify_bref` reads `lbase`/`lsize` (`io.c:3250-3273`); heal patches are `(off, bytes)`
  ranges. The degraded read rebuilds the whole column (`io.c:2057`). So detection and repair need
  nothing new for a shared column, and one reconstruct heals all its chains.
- **A put hands over the whole 64 KB buffer image** (`io.c:895`: `kmalloc(psize)` + `bcopy`) at
  the last drop of the dio. Two chains in one column would put twice (`rz_put_anomaly`, `ondisk.c:2412`)
  and the first put already makes the column "arrived" (`col_arrived`, `ondisk.c:2367`, tests
  `col_data[d] != NULL`). Sharing therefore needs a pin (11.4).
- **Space map:** one byte per slot, refcount only ever set/incremented, tested `!= 0`
  (`ondisk.c:1517, 2783, 2810, 2955`; S-d confirmed), `HAMMER2_SM_VERSION 1`,
  `SM_PAGE_SLOTS == PBUFSIZE` (`disk.h:1209-1212`). `bf_end` frees a row only if no chain marked it
  (`ondisk.c:2916-2990`).
- **Metadata mirrors go to every member** (P10 table), not a fixed 3: an all-disk mirror costs N x.

### 11.3 Options

| | (a) pack in a column | (b1) mirrored small area | (b2) 4+2 row, small unit | (c) RAIDZ variable width |
|---|---|---|---|---|
| raw cost of a 4 KB file | 1.5x + share of a column | N x (6 disks: 6x; 3 copies: 3x) | 1.5x | 3x (1 sector + 2 parity) |
| on-disk format | none for brefs; sm v2 optional | none (freemap exists) | second geometry and map | new address scheme, all of it |
| gen rows / seal | one extra pin, see 11.4 | none (metadata path) | all of §10 again, per area | none: a block owns its row |
| R5 | sub-block arrived = column put | metadata rule | as today | none (no shared parity) |
| bulkfree | column mask, holes not reused | freemap, exact 1 KB | as today | freemap-like extents |
| degraded / heal | unchanged (per chain check) | exists (`mirror_heal`) | needs dio psize != 64 KB | per block width |
| resilver (P6) / scrub | unchanged, can skip dead columns | exists (md copy) | second loop | tree walk, not row walk |
| code | moderate, all in alloc and putblk | small | large: `psize` is hard-coded 64 KB (`io.c:185, 285`) | rewrite |

- **(a) Pack several chains into one column.** One column is filled by a bump allocator in 1 KB
  units (a radix sub-allocator as in the freemap is overkill: nothing is freed in place, 11.5).
  Detail in 11.4. It keeps every invariant of §10 and never uses more space than today.
- **(b1) Mirrored small area** (freemap-allocated, 1 KB granularity, mirror like metadata). It reuses
  the freemap, bulkfree, heal and resilver code, and has no R4/R5 or parity problem. Its price is the
  copies: all members get every copy (6x on 6 disks) or the mirror must be capped at 3 (survives 2
  failures, 3x), and it brings back the P10 mirror write cost for data. Worse than (a) on space
  and bytes written, and more mirror traffic on the USB disks. Reasonable only as a fallback if (a)'s
  pin proves hard.
- **(b2) Separate 4+2 area with a 4 KB stripe unit.** 4 chains per 16 KB row, 1.5x. It needs a second
  space map, open-row tree and gen logic, and a dio with `psize` 4 KB (`io.c:185, 285` assert
  64 KB). It only helps blocks <= 4 KB; 8-60 KB (including most LZ4 results) still need a tier.
  Rejected.
- **(c) RAIDZ-style variable width.** Removes the write hole and the seal machinery for data, but a
  4 KB block costs 3x (1 data + 2 parity), the address becomes array-wide (every bref, bulkfree,
  scrub and resilver change; resilver must walk the tree instead of rows). A new format, not a
  stage. Rejected for this plan.

### 11.4 Recommendation: (a), with a pinned open column

**Allocation.** `stripe_alloc` for a v3 DATA chain of `bytes <= vfs.hammer2.raid6_subcol_max`
(default 16384; 0 = off, which is today's behaviour) takes the next 1 KB-aligned range of the
*open sub-column*; if none fits it takes a new column (as now) and opens a sub-column in it.
`data_off = phys_off + off | radix` (`bref_slot` unchanged). A sub-column is
`{row, disk, gen, next_off, npending, dio}`; there is one per (current gen, data disk) at most
and it lives on the open row (`struct hammer2_open_row`, `local_hammer2.h:1160`).

**The pin.** The sub-column holds one dio reference (`io_getblk` of the whole pbase, NEW, so the
buffer is zero-filled once). Chains in the column take their own refs and drop them as today, so the
last drop (and the `putblk` -> `add_data` put) happens exactly once, when the sub-column
**closes** and drops the pin. Close when it is full or the gen advances. `npending` counts
reserved sub-blocks whose chain has not yet dropped its dio; close waits for 0.

**"Arrived".** A column arrives when it is put, as today (`col_data[d] != NULL`): all chains
in it arrive together. The R5 rule in `chain_modify` stays textually the same (`col_arrived`). For a
chain whose column is still open the rewrite is in place when the new size fits the old range and
a fresh sub-range otherwise (a bump, no new column); once closed it is the existing
fresh-slot rule. A late chain can never join a closed column, so there is no RMW. The Part A
radix-only resize is the special case of a chain too big for sub-columns.

**Seal (§10.2).** `seal_gen` first closes every sub-column of gen <= G (wait for `npending`,
bounded by `raid6_gen_wait_ms`; a timeout fails the flush as for rows), then waits for rows as now.
A partly filled column is written at its full 64 KB, so an `fsync` of one 4 KB file costs what it
costs today. Open columns are pinned memory: <= 64 KB x (data disks) x 1 per gen.

**Rows.** A row still holds up to `ndata` columns, now of up to 16 chains each; `row_complete`,
P/Q, `dropped_mask` and `col_released` count columns and are untouched. `col_release`
(`ondisk.c`, §10.6) on a failed put zeroes the column: it must also drop every sub-block, so the
release is per column and the chains see EIO on read (same as a lost column today).

**Space map and bulkfree.** `bf_mark` marks (slot, column) instead of slot; `bf_end` frees a column
when no chain referenced it in two consecutive passes, and the row when no column is live. A
column with one live 4 KB file still pins 64 KB (status quo, never worse). Holes in a column are not
reused (that would be RMW of P/Q). Persisting the mask is S1: `HAMMER2_SM_VERSION` 2 with a `uint32`
mask per slot replacing the refcount byte (S-d), 4x the map (4 GB disk: 64K slots, 256 KB); `sm_read`
already rebuilds an invalid map by a tree walk, so v1 arrays mount and convert. It is **not
needed for correctness** (the mask is rebuildable) but lets resilver and scrub skip dead columns
(P6) and removes the refcount byte; so it is a later, separate commit.

**Degraded, self-heal, scrub, resilver.** Per 11.2: unchanged. Resilver and scrub work per row and
column. A bad sub-block is repaired by range patch; the column reconstruct fixes its neighbours too.

**Risks.** (1) The pin holds a dirty buffer across chain lock cycles; the vnode buffer
discard path (`rm`) must not invalidate it (`hammer2_io_inval` is a NOP on v3, §10.6). (2) Chains in
a column complete in any order and a flush can start while some are being written: covered by
`npending`. (3) Fragmentation after heavy deletes (bounded by status quo). (4) Both getblk NEW
zeroing and `GOOD` handling for a shared dio must be checked in `_hammer2_io_getblk` (`io.c:332-640`).

### 11.5 Commit plan and tests

1. **P2 numbers and counters** (done in Part A: `rz_cols_alloc`, `rz_resize_inplace`; P2 prints
   columns reserved). Add a P2 `df` space measure and a compressible variant (P2c). Record today's
   34-37x as the failing baseline of the new asserts, set the limit to 8x/2x only in step 4.
2. **Sub-offset plumbing, off by default:** `raid6_subcol_max=0`. A test hook places a chain at an
   offset (`vfs.hammer2.raid6_subcol_test`) to exercise `data_off` with offset on read, scrub,
   degraded read, self-heal patch and resilver. Tests: L, K, D, B, C with the hook.
3. **Open sub-column + pin + close in `seal_gen`**, with `npending` and the timeout. Default still 0.
   Tests: J, R (rewrites across sub-columns), F, I and `crash_host.sh` (kill with an open column).
4. **Turn on at 16 KB.** P2 asserts: write amp <= 8x, balance as now, space <= 2x user, P2c
   columns <= 1/3. New group Q: 4000 files, remove half, bulkfree x2, remount, every file verified;
   two-disk failure with reads; corrupt one sub-block, scrub finds and repairs exactly it; overwrite
   in the same TXG (pending and closed). N and T (reuse) for capacity.
5. **Bulkfree per column** (`bf_mark`/`bf_end`), still with the v1 map (refcount byte becomes a
   bitmask of 8 data disks, or an in-memory-only mask). Test: Q after rm leaves ~50% of columns freed.
6. **Later: sm v2 / S1 mask** and resilver/scrub skip of dead columns (P6); S3 fold-in.

### 11.6 Open questions

- Threshold: 16 KB, or 32 KB (4 chains per column)? Larger blocks waste less per column but more
  blocks go through the pin.
- Is an `fsync` of a tiny file allowed to cost a full column plus P/Q (as today), or must a sync write
  only the touched 4 KB (that needs RMW parity and returns the write hole)?
- Is the all-disk metadata mirror intended for data in (b1), or is 3 copies acceptable? (Only matters if
  (a) is rejected.)
- Do v1 space maps have to stay mountable by an older kernel (affects when step 6 may land)?
- Hole reuse inside a live column (a repack at bulkfree) is left out: does a workload with churn on
  small files matter enough to design it?
