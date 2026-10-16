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
   **Done (2026-10-15, fix/group-parity), parity part.** `tests/v3/common.sh` runs `h2stripe_check` on the unmounted members for every array a group builds: before each `setup_fresh` replaces the previous test's array, and in `summary()` for the last one (`PARITY_CHECK=0` off, `1` end of group only, default `2`). A bad row is a counted FAIL with row, P/Q disks and, when all columns are present, the suspect disk; the time per group is printed (`parity check: N runs, R rows, Ts`) and totalled by `run_all.sh`. Unmount is the quiescent point (it flushes and drains the async seal, so every sealed row is on disk). The checker (`src/diag/h2stripe_check.c`) was extended: it reads only rows the committed space map allocates; a disk the newest header does not call ONLINE is left out, and a row with one column missing is still verified (a missing data column is rebuilt from P and must agree with Q; a missing P or Q leaves the other to check the data), two missing columns are counted as degraded and skipped; it picks the newest header over all disks, not disk 0 only (the old code failed with disk 0 out); every ONLINE disk's newest `rz_txg_seq` must equal the array's (HDR_ERROR); Q is computed 8 bytes at a time. `deploy.sh` now always rebuilds it: `/root/h2diag` is shared by every worktree that deploys and tar keeps source mtimes, so a stale binary looked newer than the source (it produced false errors on a failed disk 0 in the first run). **Why not the kernel scrub (group Y) instead:** it needs a mounted array, walks the tree, skips degraded arrays (Y8), and *repairs* what it finds, which hides the bug; an offline read-only check of the media is the right oracle for "what is on disk after the flush". The scrub stays the online/repair path (group Y). **Cost** (full suite, h2dev, 4 GB disks): X 61 s of 871 s (20 arrays, 113893 rows), N 32 s of 705 s (the 2 big arrays), P 3 s, G/B/D 1 s, every other group under 1 s; about 1.8 s of 1 s-resolution rounding apart, the check is about 10% of X and N and nothing elsewhere. Rows read: 64 KB per column, only allocated rows, so on the Pi's 16 GB slices cost scales with data written, not disk size (about 1 s per 2000 rows at USB speed is an estimate, not measured). **Findings:** no real parity mismatch on a healthy or cleanly failed array in any group (A-Y, both modes). Three tests left media corrupt on purpose and were flagged by the check; each is annotated with `parity_skip` (printed as a NOTE, never silent): B6, U5 and V7 overwrite a second member's data zone, parity columns included; the degraded read heals the data columns it uses, but an unread P or Q column on that disk stays garbage until a parity scrub (group Y), so rows with a missing P and a corrupt Q (48 in B6, 1 in U5/V7) mismatch. That is expected, and a reminder that the read path does not repair parity columns.
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

**Status: done (fix/r11-hung-disk).** All bullets confirmed in the code; the second was done differently.

- **Gone task (confirmed, done).** `hammer2_disk_gone_task` runs on its own queue `h2gone`. It fails the member first (shared `rebuild_lk`, `LK_NOWAIT`, so I/O goes around the disk at once), then tries `rebuild_lk` exclusive with `LK_NOWAIT` to close it. If a lock is busy it releases `mntlk`, sleeps 100 ms and rescans from the top (at most 5 min), so it never waits for a lock with `mntlk` held and never queues as an exclusive waiter that blocks the seals.
- **`rebuild_lk` across waits (confirmed, modified).** The header loop must keep the lock until its I/O is done: it exists to keep `close_member` / `vinvalbuf` from running under a buffer being written. Instead every wait inside it is now bounded (below), so the hold is bounded by `io_timeout_ms` per phase, and the gone task no longer queues for it.
- **Unmount (confirmed, done).** `hammer2_close_devvp` skips the cache FLUSH and V_SAVE for failed members. The FLUSH for a healthy member is timed, and a timeout also drops V_SAVE.
- **`stripe_next_disk` (confirmed, done):** now `u_int`, taken modulo `ndisks` unsigned.

**I/O timeout (`vfs.hammer2.io_timeout_ms`, default 30000, 0 = off).**

Nothing can take a bio back from a driver, so the design never cancels or frees anything the driver holds. Two mechanisms:

1. *Timed waits for explicit bios* (P/Q and mirror writes, header writes, cache flushes, space-map pages, resilver writes, sibling pbuf reads). `hammer2_bio_timed_init` installs `hammer2_biodone_timed`; `hammer2_biowait_timed` is `biowait_timeout` plus a hand-over word in `bio_caller_info3` (0 running, 1 completed, 2 abandoned). On a timeout the waiter moves 0 to 2, returns `ETIMEDOUT` and **never touches the buffer again**; when the driver finally returns the bio, the completion sees 2 and releases the buffer (`brelse` with `B_INVAL|B_NOCACHE`, or `relpbuf`). A completion that wins the race makes the waiter finish normally. Callers already turn a write error into `hammer2_raid6_write_failed`, so `ETIMEDOUT` fails and persists the member through the R3 helper, the P/Q write of a seal returns and the chain locks are released. Cost: a buffer (and its 64 KB) stays locked until the driver returns it, at most the number of commands in the drive's queue, and no more are issued once the member is failed.
2. *Watchdog for reads and delayed writes.* `bread`/`cluster_readx` and `VOP_FSYNC` cannot be timed. Per member `iowd.nwait` counts reads waiting on the device (the buffer is `getblk`ed first, so a wait for a buffer lock does not count) and the sync's wait for the member's write track; each completion refreshes `iowd.stamp`. A per-mount callout (1 s) fails (`HAMMER2_FAIL_DEFER`) a member with `nwait > 0`, bios outstanding on its device vnode, and no completion for `io_timeout_ms`. The sync waits for a member's write track itself (1 s slices) and stops waiting once the member is failed, so one hung drive no longer holds the flush. A failed member is not `vinvalbuf`ed while bios are still outstanding on it.

*Which readers stay stuck.* A reader already inside `cluster_readx` on the hung disk stays there until the driver returns the command: its buffer holds the same (vnode, offset) any reconstruct would `getblk`. Everything else goes degraded as soon as the member is failed. That reader is bounded by the driver (da: 60 s per attempt and retries), not by us; uas resets (about 3 s) fail all outstanding commands, which also releases it.

*Default.* 30 s is above any legitimate stall on these drives (a SYNCHRONIZE CACHE of a full write cache takes a few seconds, a uas reset about 3 s) and below the first da timeout (60 s), so a drive that is truly silent is failed before CAM starts its retry cascade (minutes). On a hard disk with spin-up, raise it.

*Gaps.* Not timed: resilver reads (`rebuild_lk` exclusive: a hung survivor stalls the resilver and the writers), mount-time reads, the blocking metadata mirror `bread`, and `ioctl` header rewrites. The injected delay is a sleep in hammer2 before submit (`inject_delay_ms`); the watchdog counts it like a stalled bio, which is how W4/W5 exercise the same fail path as a real hang. Real-driver behaviour (a bio that stays in the driver) is covered by reasoning only: the abandon hand-over above is the part tests cannot reach.

**Disk gone under load: `vinvalbuf: flush failed` panic and a hang (fix/gone-close, 2026-10-13).** The Pi panicked in `hammer2_disk_gone_task` -> `close_member_locked` -> `vinvalbuf` when the USB disk holding member 0 vanished during group N. Reproduced on h2dev with the test hook `vfs.hammer2.inject_gone=<disk>` (runs the real gone handler for a member; the x86 guest has no `disk_gone` event) under a write + metadata-churn + read load (group X, X9). On the old code the close does not panic there but **hangs**: `h2gone` sleeps forever in `vinvalbuf` ("vinvalbu"), the flusher in `h2trans`, `bufdaemon_hw` in `h2wpipe` (2 of 2 runs, at +1 s after the start of the load, disk 0).

Cause: the rule "every `getblk` on a member devvp happens under shared `rebuild_lk`" holds for the P/Q, mirror, space-map and header writers, but not for the dio layer, which is where disk 0 differs (it is the primary of every metadata block, so its devvp carries every dio buffer):
- `_hammer2_io_getblk` (cold reads `io.c` devread/spread, NEW/NEWNZ `getblk`, the degraded pre-emption `getblk` on the failed devvp, and the `getblk` that follows an error-triggered `mark_failed`) takes no `rebuild_lk`; `vn_lock` on the devvp does not stop `getblk`. A buffer created after `vinvalbuf`'s scan, while `vm_object_page_remove` sleeps, is the panic.
- A dio keeps its buffer locked (`BUF_KERNPROC`) while a chain holds it, and a modified chain holds it until the flush. `vinvalbuf` sleeps for such a buffer (`LK_SLEEPFAIL`) under exclusive `rebuild_lk`, and that flush needs `rebuild_lk` shared (volume header, mirror writes): deadlock. The sync's own `vinvalbuf` of a non-writable member (`flush.c`) had the same two problems (it waits for buffers its own flush holds).
- The close slept in `vn_lock(devvp)` under exclusive `rebuild_lk`; the syncer holds the devvp locked while it writes dirty buffers (waiting for a flush, which waits for `rebuild_lk`): a third deadlock, found when the first two were fixed.
- The repair thread and read_sibling/bufcache also `getblk` a member devvp without the lock (they cannot take it: the parity scrub drains the repair queue under exclusive `rebuild_lk`); they are stragglers only.

Fix: no waiting and no panic in the close. `hammer2_devvp_purge()` (io.c) invalidates the unbusy clean and dirty buffers with `RB_SCAN`/`BUF_LOCK(LK_NOWAIT)` (as `vtruncbuf` does), skips busy ones, repeats while it finds some, then removes the VM pages; it returns `EAGAIN` and does nothing while bios are in the driver. A busy buffer is released clean by its dio (`putblk` never writes to a failed member), so skipping it is safe; a buffer made after the purge is clean and harmless on a closed devvp (the same state as any closed failed member). `close_member_locked` uses it, takes the devvp lock with `LK_NOWAIT` and returns `EAGAIN` for both; the gone task retries it with `mntlk` released (its existing loop), the replace ioctl and `close_member` drop `rebuild_lk` and try again. `flush.c` uses the purge instead of `vinvalbuf`. The reason for the close is kept: the member is still failed, then `VOP_CLOSE`d so that the driver can free the disk. Not changed: the unmount `vinvalbuf` (`hammer2_close_devvp`; no I/O is running then).

Test: group X9 (`inject_gone` on disks 0, 2, 3 at several delays under load: closed, data intact, replace onto the same path, scrub clean). Not verified: a real unplug (USB), only the handler on a live disk; the Pi must run N with a pulled disk.

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

**Step 3 measurement (async sealing; branch fix/async-seal, h2dev 4 x virtio, 2+2).** Counters
`vfs.hammer2.rz_pq_wait_ns/_cnt` (time `write_row` waits for the P and Q writes) and
`rz_seal_ns/_cnt` (time inside `row_io`, rebuild_lk included) are summed over all threads; test P
prints them per workload and `tests/v3/seal_par.sh` runs N parallel writers.

| workload | rows | P/Q wait | in seal | wall | wait / wall |
|---|---|---|---|---|---|
| P1 256 MB seq | 2048 | 366 ms | 1524 ms | 8.0 s (4.6-5.3 s in two later runs) | 4.6% (up to 7%) |
| P1c 256 MB, LZ4 2x | 1024 | 442 ms | 710 ms | 3.7 s | 12% (one run) |
| P2 4000 x 4 KB | 125 | 273 ms | 307 ms | 5 s | 5.5% |
| P2c | 33 | 8 ms | 20 ms | 4 s | 0.2% |
| 1 writer, 128 MB | 1024 | 185 ms | 449 ms | 3.25 s | 5.7% |
| 4 writers x 64 MB | 2048 | 630 ms | 1173 ms | 3.29 s | 19% summed, 4.8% per thread |
| 8 writers x 32 MB | 2048 | 990 ms | 1559 ms | 3.60 s | 27% summed, 3.4% per thread |

Per row the wait is 0.18-0.45 ms on virtio. Wait is a thread-time sum, so it is an upper bound on
what async sealing can win and only if the disks would otherwise sit idle: while a sealing thread
waits, the other columns' bdwrites and other writers' rows are still in flight, so wall time is set
by the devices. On h2dev the share is under 10% for every workload except one P1c run (12%, wall
3.7 s; the same workload gave 3.3-3.4 s in later runs), so **the recommendation is not to build it
here**. On the Pi the P/Q write queues behind the data columns of the same and earlier rows on a
slow USB disk, so the absolute wait per row grows with the disk's service time; but the same holds
for the bytes, which the devices must write either way (P1 ~18 MB/s is the USB disks' limit).
Async sealing would only remove thread stalls, not device time, and costs the `read_sibling` raw
fallback, scrub, resilver, bulkfree and flush interlocks that this item was blocked on. Measure on
the Pi with `sysctl vfs.hammer2.rz_pq_wait_ns` over P1 first: build it only if the wait exceeds
about 10% of wall *and* per-disk utilization (devstat busy) is well under 100% during P1.

**Step 3 measurement on the Pi (4 USB SSDs, coordinator, kernel #16 era).** The h2dev
recommendation above does not carry over:

| workload | P/Q wait | wall | share |
|---|---|---|---|
| P1 256 MB seq (run 1) | 2710 ms | 13.71 s | 19.8% |
| P1 256 MB seq (run 2, 38.1 MB/s) | 2195 ms | 6.72 s | 32.7% |
| P1c | | | 16.8% |
| P2 | | | 5.3% |
| P2c | | | 1.9% |

A raw sequential write to all four disks in parallel runs at ~55 MB/s per disk with the CPU about
78% idle; during P1 the array writes ~19 MB/s per disk, a third of that. Both of the build
conditions (wait above ~10% of wall, disks well under 100%) hold, so the synchronous P/Q wait in
the seal path is a real throughput limit on the Pi and phase 2 is built (below).

**Phase 2: seal without the writer waiting for P/Q (`vfs.hammer2.async_seal`, default 0).**

*What changes, and what does not.* Today `row_io` computes P/Q, starts the P, Q and zero-column
writes (`hammer2_bwrite_start`, already asynchronous and already in flight together) and then
`biowait`s them under `rebuild_lk` shared. Phase 2 moves only the waiting and its error handling:
the writer starts the same writes and hands the buffers to a per-mount *parity reaper* kthread,
which does the timed wait (`hammer2_bwrite_wait`, so R11 timeouts apply unchanged), the `brelse`,
and `hammer2_raid6_write_failed()` for a failed member. Computing P/Q, the zero columns, the
rebuilding-disk copies (`p_mem`, `q_mem`, `zbuf`, `dropped` columns; all `write_nowait`, waited
inside) and `rb_wgen`/`rebuild_done` stay in the writer under `rebuild_lk`, exactly as now. With
`async_seal=0` the code path is the old one (`write_row` = start + wait, same order, same
counters).

*Who issues, who completes.* The writer (the putblk that completes the row, or the flusher in
`seal_all_open_rows`) issues. The reaper, one thread per mount (as the repair kthread), completes
in registration order. biodone is not used: `write_failed` can take locks and print, which is not
allowed in a driver completion, and a per-bio callback could not do the R11 abandon either. The
buffers are `BUF_KERNPROC`'d by `hammer2_bio_timed_init`, so another thread may `brelse` them, as
the abandon path already does. P/Q/zero buffers keep `B_RELBUF|B_NOCACHE` and are released by the
reaper (so nobody else frees them); KVABIO: the reaper never touches `b_data`, and the release
path of a KVABIO buffer does not need the mapping on the current cpu.

*Per-row tracking.* One `hammer2_pq_ent` per sealed row, kmalloc'd by the writer, holds
`phys_off`, a sequence number and the buffers with their member indices. It is put on
`hmp->pq_list` (spinlock `pq_spin`, registration order, `pq_seq++`) **before** any P/Q work, with
`rebuild_lk` already held shared, and removed by the reaper after the last buffer is released, which
then sets `pq_done_seq = seq` and wakes waiters. Completion is strictly in registration order, so
"entry E is done" is the same as `pq_done_seq >= E.seq`. The row's `rz_sealing` reference passes to the entry
and is dropped by the reaper, so every existing `rz_sealing == 0` interlock (S6 parity scrub) now
also means "no parity in flight".

*Primitive.* `hammer2_pq_wait_slot(hmp, phys_off)`: under `pq_spin` find the highest seq among
entries with that `phys_off` (any member: P, Q and the zero columns are all parity-side) and sleep
until `pq_done_seq` reaches it; no pointer is kept. `hammer2_pq_drain(hmp)` waits for
`pq_done_seq >= pq_seq` as read at the call (no starvation). Both return at once when the list is
empty (one unlocked load).

*Back-pressure.* A writer waits (on `pq_done_seq`) before registering while `pq_count >=
vfs.hammer2.async_seal_max` (default 16 rows, so at most 16 x (P+Q+zero columns) x 64 KB, about 3 MB;
each buffer is also under `runningbufspace` throttling through `bsetrunningbufspace`). The reaper
never takes a lock a writer can hold and writes complete by themselves, so the wait cannot
deadlock (the old note about a bawrite deadlock was unbounded accumulation of buffers the writer
itself had to release; here the writer never owns them after the hand-off).

*Every reader of parity, and why it is safe.*
- `read_sibling` (and so `read_degraded_x`, self-heal reconstruction, resilver's non-batch reads):
  calls `pq_wait_slot` for the column's offset first, **before** both the cache path and the raw
  pbuf fallback. The fallback is the dangerous one: an in-flight parity buffer is busy, so the
  `GETBLK_NOWAIT` fails and the raw media read would return the *old* parity. After the wait the buffer is
  gone (NOCACHE) and the media is current. A caller may hold a DIO buf of a sibling column or
  `rebuild_lk` exclusive while it waits: the entry's writes need neither (the writer registered under
  `rebuild_lk` shared, so an exclusive holder only ever sees entries whose writes are already
  issued; the reaper takes no lock). The window between unlink and registration (writer still
  waiting for `rebuild_lk`) exists today and is not widened: such a row is not committed and no
  committed data depends on it.
- Scrub parity pass (S6): the batch read is unlocked and may see stale media; the existing
  protocol (a mismatch is verified under `rebuild_lk` exclusive with `rz_sealing == 0`, re-checked
  after the read) is unchanged and now also covers in-flight parity because the entry holds
  `rz_sealing`. Busy parity buffers already make `scrub_pq_col_stable` send the column through the
  cache. Repair writes happen under the same lock and condition.
- Resilver Phase A/B and the resume cursor: every blocking `rebuild_lk` exclusive section that reads
  survivors (window loops, replace/close in ioctl) calls `pq_drain` right after taking the lock
  (new rows cannot start while it is held). Rows sealed after `rebuild_active` are `rebuild_done` and
  skipped; rows sealed before it are covered by the drain, and by the sync_pmp (flush) that precedes
  the passes. The resume cursor is written only after the pass's writes are waited; the
  rows-below-cursor are never parity-dependent on anything in flight because a cursor reaches
  media only in a volume header, which is after the flush drain.
- Bulkfree freeing a row and row reuse: bulkfree only frees what no committed tree references and
  the flush that committed the previous TXG has drained all parity, so an in-flight row is never
  freed; and if a slot were reused while an old entry is in flight, the new P getblk waits on the
  old busy buffer (same offset), and `pq_wait_slot` takes the max seq. `rz_free_epoch` is untouched.
- R1 three-phase flush: after `seal_all_open_rows` (so every row of the closing generation has been
  issued: the writers that sealed them finished their transactions, and the flusher sealed the
  rest itself) the flush calls `pq_drain` before the first VOP_FSYNC. The three phases then run as
  before: parity completed, FSYNC/FLUSH CACHE on every member, volume headers. A header can therefore
  not overtake a P/Q it commits. A test-only counter `pq_hdr_violations` is bumped if the header
  stage finds an entry registered before the flush's drain point not done.
- Errors: the reaper calls `hammer2_raid6_write_failed(disk, err)` for each failed buffer, ETIMEDOUT
  included, in its own context (it holds no lock; `write_failed` was never required to be called
  under `rebuild_lk`; `close_member` and replace wait with the drain). The member is failed a little
  later than before, but a row that sealed with a failed-pending P/Q is still committed only after
  the drain, i.e. after the failure was recorded, and the flush then writes the headers to the
  surviving members, as for a sync-detected failure. Redundancy-exceeded return values were already
  ignored (`(void)write_row`).
- Member close/replace/disk-gone: `rebuild_lk` exclusive used to wait out writers that were waiting for
  their P/Q. The blocking takers now drain right after acquiring; the non-blocking taker (disk_gone,
  with `hammer2_mntlk` held) lets go and retries if entries are in flight.
- Unmount and `umount -f`: `hammer2_pq_stop` (before the repair thread stops, after nothing more
  can seal) drains, terminates the reaper and frees leftovers; the final flush has drained already.
  A hung member costs at most its R11 timeout per buffer, as before.

*Not changed:* open-row columns, `rz_sealing` users other than the hand-off, data columns, metadata
mirrors, layout, on-disk format.

*Test hook:* `vfs.hammer2.inject_pq_defer_ms` makes the writer hand the P/Q/zero buffers over *unissued*
and the reaper start them after that many ms, so the media holds old parity for a controlled time
while the entry is registered. Group O uses it (plus `pq_inflight`, `pq_probe`/`pq_read` hooks)
for the reader, flush, error and unmount races.

*Error reporting decision:* the reaper must not call `auto_fail_disk` (it takes the voldata lock, which a
reader of the same row may hold while waiting for the reaper). A failed P/Q write therefore calls
`mark_failed(HAMMER2_FAIL_DEFER)` (in memory at once); the flush persists it (`persist_failed`) after the
drain, before the volume header, so a member failed by the reaper is on the media before a header that
depends on it.

*Results (h2dev, x86, 4 x vbd; Pi not measured by this branch).* async_seal=0 / 1, same build:

| | 0 | 1 |
|---|---|---|
| P1 (256 MB) | 50.6 MB/s, P/Q wait 550 ms, 10.9% of wall | 59.4 MB/s, wait 0 ms |
| P1c | 39.4 MB/s, wait 397 ms (6.1%) | 74.0 MB/s, wait 0 ms |
| P2 / P2c wait | 50 / 60 ms | 0 / 0 ms |
| write-amp P1 / P1c / P2 / P2c | 2.023 / 1.018 / 3.77 / 2.22 | 2.023 / 1.015 / 5.05 / 3.62 |

Write-amp for P2/P2c varies run to run with metadata flush timing (the data columns and P1/P1c are
unchanged); the 1 run per mode is not enough to call P2 a regression. The async=0 P1c throughput target
(>= 90% of P1) failed once on the VM (noise: 39 vs 51 MB/s); async=1 passed it.
Group O (new, 37 checks) and A B C D F K L R W X Y Z pass with async_seal=1; see the commit message
for counts.

*Pi results (k43, 75a8cdb, 4 x USB SSD, 2026-10-13).* async_seal=0 / 1:

| | 0 | 1 |
|---|---|---|
| P1 | 18.7 MB/s, P/Q wait 2114 ms (15.5% of wall) | 43.4 MB/s, wait 0 ms |
| P1c | 32.1 MB/s, wait 1309 ms (16.4%) | 32.7 MB/s, wait 0 ms |
| write-amp P2 / P2c | 4.69 / 3.96 | 5.48 / 3.60 |

X 107/107 and W 44/44 pass with async on. O failed O2 twice only because the Pi's sed was broken
(arm64 TRE regex, fixed in the fork as c026f0c124 and deployed to the Pi on 2026-10-13); the data
checked out (deg crc == col crc, reader waited 4901 ms).

*P1c: where the time goes (h2dev, ns per 64 KB block, summed over the writer thread; new counters
vfs.hammer2.cw_*, printed as "INFO ... write path" in group P).* The compressed path runs in the
per-inode strategy xop thread (one thread for a single file), serially per block: zero test, LZ4,
`hammer2_assign_physical`, `io_newnz` + copy + setcheck + dedup record + bdwrite, `subcol_done`
(row seal submit). Before: P1c zero 15 us, **lz4 254 us**, assign 43, buffer 22, done 156 (async 1).
P1 skips LZ4 after the heuristic trips (548 of 4096 blocks tried) and pays 27 us for it.
`LZ4_compress_limitedOutput()` does a `kmalloc(16 KB)` + `kfree` of the hash table on every call; 16 KB
is above the slab zone limit, so every block is a kmem_slab (VM map, pmap) round trip. That was about
a third of the LZ4 time on x86 (254 -> 170 us after the fix) and more on a 4-core A72 where the
cross-CPU part of a map operation is slower. The rest of the 170 us is LZ4 itself (about 385 MB/s on
this host). Compressed blocks do not defeat row packing (1024 rows for 4096 x 32 KB chains, 2 per
column, balanced); the extra "done" time per block is the row seal (4 blocks per row instead of 2).
Fix: the hash table now lives in the tail of the compression buffer (`cache_buffer_write` objects are
32 KB + `HAMMER2_LZ4_CTXSIZE`) and `LZ4_compress64k_heap_limitedOutput()` is called directly; no
allocation per block, on-disk format unchanged. h2dev before/after, async_seal=1: P1c 56.3 -> 74.2-80.5
MB/s (P1 42-55 in the same runs); async_seal=0: 65.3 -> 65-67.5 (P1 52-66). Group P also checks now that
P1/P1c read back identical after a remount, and fails instead of measuring when the source file could
not be written (the guest's / filled up and P1c once "ran" at 400 MB/s on a 9 MB file).

*Open: compression is still serial and on the critical path.* After the fix the writer thread spends
about 430 us per block (x86) on a P1c block, 51% of the wall time; at 3x slower per block (A72) it is
the limit again if the fix does not remove enough. If the Pi still shows P1c < 90% of P1, the next step
is a design, not a tweak: compress on a small per-mount worker pool (or per CPU) ahead of
`assign_physical`, keeping chain allocation and the row fill in the xop thread so ordering and the
sub-column layout stay as they are. Measure with the "write path" INFO line first (lz4 ns/block vs
the wall time per block: 8 s / 4096 = 1.9 ms on the Pi).

*Pi results (k46 = k44 overlay + bcmmbox clock sysctls, 2026-10-13).* The Pi firmware leaves the ARM
at 600 MHz unless the OS asks for more; every earlier Pi number was measured at 600 MHz. The new
`dev.bcmmbox.0.arm_mhz` (RW) / `arm_max_mhz` (1500) set it at runtime; it reverts at reboot. At 600 MHz
LZ4 cost 877 us/block and P1c was 34-35 MB/s in both modes. At 1500 MHz (md5 of 512 MB: 5.17 -> 2.12 s
user; SoC about 61 C, never throttled), group P four times alternating async_seal 0,1,0,1 after a
warm-up run:

| | async 0 | async 1 |
|---|---|---|
| P1 | 61.1, 61.5 MB/s (P/Q wait 1.8 s, 43% of wall) | 86.2, 85.3 MB/s (wait 0) |
| P1c | 73.4, 67.4 MB/s | 77.1, 72.1 MB/s |
| lz4 ns/block (P1c) | about 370 us | about 370 us |

The first P1 of every sequence is slow (15.9-19.5 MB/s) whatever the mode, which is what made k43/k44
look like a 2x async win on P1; it is SSD state after the previous run, not async. With that excluded,
async_seal=1 gives P1 +40% and P1c +5-7%. Group P then fails one check: P1c >= 90% of P1 (72-77 vs
85-86, 85-90%). P1c is now CPU-bound in the single strategy thread (4096 x 370 us = 1.5 s of 3.3 s), so
the compression worker pool above is what would close it; the check itself is right.

*LZ4 worker pool (fix/lz4-pool, 2026-10-14).* The design above, as built; all of it in
`local_hammer2_strategy.c`.

- *Where.* `hammer2_strategy_write()` (the VOP_STRATEGY caller, so the thread that flushes the
  buffer) decides per bio whether to start a job and stores it in the xop (`xop->cjob`). The
  job holds the bio data pointer (workers read `bp->b_data` in place: the bio cannot complete
  before the xop thread has consumed the job), one `cache_buffer_write` object (32 KB output +
  LZ4 hash table in its tail, so still no per-block allocation) and a `struct task`. The pool is
  one taskqueue (`h2comp`) with min(ncpus, 4) kthreads spread over the CPUs, created in
  `vfs_init`, freed in `vfs_uninit`; taskqueue is already used there for `h2gone`. The worker
  runs the all-zeros test (when the check code is on) and `LZ4_compress64k_heap_limitedOutput()`,
  stores `comp_size`, sets `done` and `wakeup()`s. The xop thread, at the point where it used to
  test for zeros and call LZ4, waits for the job (`tsleep_interlock`/`PINTERLOCKED`), takes the
  compressed buffer and carries on into `hammer2_assign_physical`, the buffer fill, the dedup
  record and `subcol_done` exactly as before. Chain allocation, row fill and block order stay in
  the xop thread, so the media layout is unchanged (P1c columns/write-amp identical).
- *Only 64 KB LZ4 blocks.* A job is started only when the inode's algorithm is LZ4, there is one
  cluster member, the block is a full 64 KB physical block (not an EOF tail, not inode-embedded
  data) and `comp_heuristic < 8` (or `always_compress`). The 1-in-8 probes of an incompressible
  file and everything else run inline. The xop thread re-validates the job (pblksize, algorithm,
  check code: `setcomp`/truncate may have changed since submission) and falls back to inline
  compression if it no longer matches. `comp_heuristic` is still updated only by the xop thread.
  A worker that finds a zero block returns without compressing and the xop thread takes the
  usual `zero_write` path.
- *Bounds and fallback.* `vfs.hammer2.comp_workers` (RW, default min(ncpus,4), clamped to the
  threads started at load; **0 = inline exactly as before**) and `vfs.hammer2.comp_depth` (RW,
  default 128, jobs in flight = bios already queued to a strategy xop; must exceed
  `flush_pipe` (100), which already bounds queued bios, or most blocks fall back inline: with 32
  only 1500 of 4096 blocks used the pool). Memory is at most `comp_depth` x 48 KB. Allocation
  failure or a full pool simply compresses inline (`cw_pool_inline`).
- *Lifetime.* Every job is consumed by exactly one xop run (also on the `finished` early
  returns), which waits for the worker, returns the buffer and frees it. Unmount drains
  `lwinprog` (the strategy xops) before the flush, so no job outlives its xop; `vfs_uninit`
  runs only after all mounts are gone and `taskqueue_free()` waits for a running task.
  `vfs.hammer2.comp_inflight` is the live count (0 after every unmount in tests P6/P7).
- *Counters.* `cw_lz4_ns` / `cw_zero_ns` now count worker time (so "lz4" per block grows to
  250-265 us in the pool runs on h2dev: it runs concurrently on busy vCPUs); new
  `cw_wait_ns` (time the xop thread waited for a result), `cw_pool_jobs`, `cw_pool_inline`.
  The group P "write path" line prints `comp-wait` ns/block and the pool job count.
- *Tests.* Group P: `P6` five files written in parallel with LZ4 (compressible, random, zero
  blocks mixed), remount, sha256 of each, `comp_inflight` 0, scrub clean; `P7` umount of dirty
  compressible data (flush compresses), data intact; `P7b` `umount -f` while four writers are
  compressing: returns, nothing in flight, array mounts and scrubs. `P_ONLY=P1` runs only
  P1/P1c, `P_ONLY=P6` only P6/P7.

*Results (h2dev, x86 4 vCPU, 4 x vbd, async 0 / 1, workers 0 vs 4).* P1c is not CPU-bound in the
xop thread on this host, so the pool shows up in the counters, not in MB/s. Write path per 64 KB
block, async_seal=1, P1c: inline zero 14 + lz4 176 + assign 38 + buffer 18 + done 172 us (xop
thread busy about 418 us per block, 42% of the 4.2 s wall); pool: assign 47 + buffer 22 + done
178 us, **comp-wait about 1 us per block** (3 ms over the whole run) = about 250 us per block
(-40%), the 266 us of LZ4 now overlap on the workers. Throughput (4 alternating runs, first run of
the sequence dropped; MB/s, P1 / P1c):

| | P1 | P1c |
|---|---|---|
| async 0, workers 0 | 45.6, 42.2, 42.9 | 72.9, 76.2, 70.7 |
| async 0, workers 4 | 63.1, 45.8, 35.8 | 63.7, 52.0, 46.4 |
| async 1, workers 0 | 41.8, 61.4, 48.8 | 58.4, 61.7, 63.1 |
| async 1, workers 4 | 56.9, 50.1, 53.6 | 60.0, 73.8, 47.1 |

A second sweep (async 0, P1c only, workers 0/1/2/4, 2 runs each) gave 67.2 62.6 / 63.1 72.5 /
64.8 60.4 / 71.7 57.4. Run to run spread (35-76 MB/s for the same configuration) is larger than
any pool effect, and P1c >= 90% of P1 holds on h2dev with or without the pool because P1 is the
slower one here (disk bound, 2x write-amp). So h2dev proves correctness and that LZ4 is off the
xop thread; whether the Pi reaches P1c >= 90% of P1 (72-77 vs 85-86 before) has to be measured
there (`vfs.hammer2.comp_workers` 0 vs 4, 2, 3 are the knobs; 4 workers + the xop thread + the
dd front end share four A72 cores).

Full suite on the final build (`comp_workers` 4, `comp_depth` 128; groups X Z R G A B C D E F H I J K L N O P Q S T U V W Y, P5 asserts on): async_seal=0 **638 pass / 0 fail**, async_seal=1 **638 / 0** (group P, with the new P6/P7 and the P1c >= 90% check, 100-104 s).

*Results (Pi k48, 1200 MHz, 4 x USB SSD, async_seal=1, 2026-10-14).* `P_ONLY=P1`, alternating
`comp_workers` 4 / 0 after a warm-up run (MB/s):

| | P1 | P1c | P1c >= 90% of P1 |
|---|---|---|---|
| workers 4 | 69.2, 68.8, 70.3 | 85.6, 81.0, 83.4 | pass (3/3) |
| workers 0 | 69.9, 74.2, 70.1 | 60.5, 58.9, 58.3 | fail (3/3) |

On the A72 the xop thread was CPU-bound on LZ4 (460 us/block inline); with the pool it waits 0 us
for results and P1c is 35-40% faster. Full group P with workers 4: 34/34 (P6, P7, P7b included).

*Defaults on (k49 = 77d89ab, 2026-10-14).* async_seal now defaults to 1 and comp_workers to 4.
Pi full suite at 1200 MHz with the defaults: **638 pass / 0 fail**, no under-voltage during the
run, SoC at most 69.6 C. k49 is the Pi's kernel8-good.img.

*P2/P2c write-amp: noise, not async (h2dev, 8 runs per mode, alternating, 2026-10-13).* `P_ONLY=P2`
runs only P2/P2c of group P. Write-amp, async 0 / 1 (mean, range):
P2 4.50 (3.97-5.15) / 4.50 (3.90-5.19); P2c 3.06 (2.41-3.38) / 3.04 (2.28-3.71). The first 4 runs
were a clean deploy (P2 4.57 / 4.58, P2c 3.22 / 3.20); runs 5-8 shared the guest with another
agent's kernel and are only supporting. Every counter is identical between modes: columns reserved
251 / 63-64 (P2 / P2c), seal rows 126 / 32-33, mf_sync 4 (3 once, async), mf_defers about 24.5k;
dc_trigger differs only in the first pass of a run. What moves the bytes is mf_blocks (the metadata
mirror copies of the TXG passes, 155-222 per run in both modes): written MB tracks it, and it
varies 40% run to run with when the syncer and the dirty-chain trigger fire against the copy. Rows
are sealed identically (same count, same columns), so rz_sealing, pq_count and the reaper change
nothing about what is written; they only remove the P/Q wait (0 ms vs 11-170 ms). The earlier
single-run differences (3.77 vs 5.05 etc.) lie inside this spread. Not a bug; no code change.

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

**Outcome, resume cursor and allocated-only Phase A (branch fix/p6-resume).** The batching, XOR
and windowed Phase A part is in `e098809`; this finishes the other two items.
- **Resume cursor.** A *resume record* lives in `raid_config` (carved from `reserved[]`, header
  stays 512 bytes): `rz_rb_id` (random id of the target being filled, 0 = none), `rz_rb_disk`,
  `rz_rb_cursor` (first slot not rebuilt), `rz_rb_cwgen`, and `rz_rb_wgen`. It is in the volume
  header, so the normal sync writes it to every writable member, the target included. The
  resilver commits it every `vfs.hammer2.resilver_ckpt_slots` slots (default 2048) by setting the
  cursor in `voldata` and calling `sync_pmp(MNT_WAIT)`.
  - *Durability order.* The cursor is set only after the pass's target writes have completed
    (`write_wait`), and it reaches media only in a volume header write, which the sync issues
    after FLUSH CACHE on every writable member (the target is one while `rebuild_active`; same
    ordering as R1/P10). So a cursor on media means the slots below it are durable on the target.
    Rows sealed during the resilver write the target's column themselves, also before that flush.
  - *Invalidation.* (1) Same target: Phase 1 stamps `rz_rb_id` into the target's four volume
    headers; the replace reads the target's header 0 and resumes only if its id equals the
    array's. A wiped or different disk has none (or an older id) and starts at 0 under a new id
    (X7d). (2) Array generation: `hmp->rb_wgen` counts rows sealed (`row_io`) while a member is
    failed and not being rebuilt: writes the target would have missed, possibly below the
    cursor. It is persisted with the header (`rz_rb_wgen`, written next to `rz_sm_gen`) and
    the record is valid only if `rz_rb_cwgen == rb_wgen`. A write while the disk is out
    therefore restarts at 0 (X7c). Over-counting (two failed members) only invalidates.
    Frees do not matter (they write nothing to the slot); a reused slot is a new seal.
  - *Trigger.* Automatic at the next `raid replace` of that slot with the same device: no mount
    time work, no new ioctl. Phase A is not resumed (it is now small, see below) and always
    reruns; Phase 3 starts at the cursor. Progress (`stripes_done`) starts at the cursor and the
    log says `resilver disk N resumes at slot S of T`.
  - *Cleared* when the replace completes (`rz_rb_id = 0`). It is left as is after a failed or
    aborted resilver. `DISK_REBUILDING` / `FLAG_REBUILDING` stay unused on purpose: the disk
    stays FAILED until complete (reads and the allocator treat it as failed), and the record,
    not a state, says "partly rebuilt". S-e can drop them.
  - *Test hooks:* `resilver_stop_slots` (EINTR after N slots, as a crash would; the disk stays
    FAILED) and `resilver_ckpt_slots`; counters `resilver_resume_slot`, `resilver_md_bytes`.
- **Phase A, allocated metadata only.** `resilver_skip_unalloc` covered only Phase 3 (unallocated
  data slots); Phase A copied the whole zone. It now also gates Phase A: new
  `hammer2_freemap_md_alloc_map()` reads the freemap leaves for each metadata extent and returns a
  bit per 64 KB chunk with any allocated (non-free, so POSSIBLY FREE too) 16 KB block; space below
  `allocator_beg` (static newfs blocks) and unreadable leaves count as allocated, a missing leaf
  as free. Only those chunks are copied (the per-zone reserved segments, which hold the freemap
  blocks, are still copied whole). `skip_unalloc=0` copies everything, as before. Blocks allocated
  after the map is taken are written to the new disk by their own flush (`rebuild_active` is set
  before Phase A), so the map need not be exact. Unallocated chunks on the target hold whatever
  the disk had: nothing reaches them (failover reads, mirror reads and scrub follow blockrefs,
  and a new allocation is written before it is read), which X8 checks by growing the tree on the
  rebuilt disk and reading all metadata with another disk out.
- **Results (h2dev):** metadata zone 96 MB, Phase A copied 8.4 MB (skip_unalloc=1) vs the whole zone (skip_unalloc=0). X 83/83, R 18/18, D 44/44, G 10/10, K 10/10, A 5/5. X7a/b resumed at slot 3280 after 1504 rebuilt; X7c/d restarted at 0. A real `umount -f` (X2) hit the resilver before its first checkpoint, so resume after umount -f is covered by the stop hook plus X7b (umount, mount), not by X2.
  X7a-d (resume, remount, stale cursor, wiped disk) and X8/X8b added to group X.

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
- **S-d. `stripe_row_refcount` is never decremented.** It is only set to 1, ++ (saturating) or 0, and only tested for ≠ 0, so it is an 8× larger bitmap, including in the persisted space map. Persist a bitmap, or repurpose the byte (S1). **Done (simplify-2):** the in-memory refcount array is gone; `stripe_bitmap` was already set iff refcount != 0, so it is the only state. The persisted byte per slot (v1 format) is written as 0/1 from the bitmap and read as nonzero; no format change. The 8x larger persisted map remains until S1 (`SM_VERSION` 2).
- **S-e. Dead code:**
  - `hammer2_io_raid6_write_scratch` (no callers);
  - the `open_row_pick` prototype;
  - the unreachable failed-disk branch at `io.c:454–473`;
  - the `absent_data` path (if R10/U confirms it is unreachable);
  - unused `DISK_REBUILDING/SPARE/ABSENT` and `FLAG_REBUILDING` constants (either persist REBUILDING during a resilver or drop them);
  - the `raid6_pack_open_rows` bisect knob, whose 0 mode is untested.
  - **Status (simplify-2):** all of these are already gone from the code except the `DISK_REBUILDING/SPARE/ABSENT` and `FLAG_REBUILDING` constants, kept because they document on-disk values and `src/diag/h2stripe_check.c` decodes them. Also merged: the three pbuf read starts (`read_sibling`, resilver, scrub parity pass) into `hammer2_pbuf_read_start`, and the two column write start/wait pairs (`write_flags`, resilver) into `hammer2_col_write_start/wait`.
- **S-f. Scrub should call the self-heal helper** instead of its own weaker reconstruction (R-minor). That removes one of the three repair implementations. **Done 2026-10-12 (a05155e):** scrub calls `hammer2_raid6_selfheal_data` and waits for the repair thread (`hammer2_io_repair_drain`); the old one only reconstructed one erasure, wrote raw to a failed disk and did not check the slot. M3 (two disks corrupted) failed before (bad=96 repaired=24) and passes now. Scrub still never checks P/Q (S6).
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
  **Done 2026-10-13 (fix/s6-parity-scrub):** `hammer2 raid scrub` now runs a parity pass after the tree walk, ON by default (`-t` skips it, `-p` skips the tree walk). It syncs, then reads allocated rows 16 at a time on all members in parallel (timed pbuf reads, at most 64 pbufs), recomputes P and Q with the normal raid6 math and compares. Columns that are dirty or busy in the buffer cache are marked suspect. Every candidate row is re-verified under `rebuild_lk` EXCLUSIVE with `rz_sealing` == 0 (new counter of rows unlinked from open rows whose `row_io` is not finished), so a row mid-seal is never "repaired". Localisation is the syndrome z = log(Qsyn/Psyn); a data column is rewritten only if every chain it changes verifies with `hammer2_bref_check_match` (one extra bulksnap tree walk, paid only when candidates exist); P-only and Q-only mismatches are rewritten from the data; a row with no single bad column is rewritten from the data only if every chain verifies, otherwise counted unlocatable. Repairs go through the repair queue, whose hash guard (current column bytes vs the bytes the scrub read, in `hammer2_io_repair_execute`) runs with no row lock and is not enough alone, so every write goes through `scrub_pq_write()`: it takes `rebuild_lk` EXCLUSIVE with `rz_sealing` == 0, requires the row still allocated and not open and `rz_free_epoch` unchanged since the verify read, queues and drains inside the lock. `rz_free_epoch` is bumped where bulkfree clears a slot bit (the only place one is cleared, and a slot must be cleared before it can be allocated again), so an unchanged epoch means the verified row is still the same allocation, and no seal can write while the lock is held. Bulkfree does run concurrently with scrub (it works on a bulksnap), hence the epoch; group Y9 runs scrubs against bulkfree and row reuse. A degraded or rebuilding array skips the pass. Counters: ioctl fields `rows_checked/parity_bad/parity_repaired/parity_unlocatable/rows_skipped`, sysctls `vfs.hammer2.scrub_pq_*`; old output fields unchanged. Tests: group Y (24/24; P, Q, zero column of a partial row, data column by `-p`, data+P, concurrent load, degraded). K L M J Q A pass (10, 15, 5, 9, 22, 5). The first run panicked (`brelse: free buffer onto another queue`: findblk() leaves the buf on its queue; fixed with bremfree). Throughput (h2dev, 2 GB data, 4 x 2 GB-ish members, warm): tree only 2-3 s, parity only 2-4 s (4 GB raw read, 16386 rows), both 5 s. Risks: extra full sequential read by default; the review's claim that the tree walk alone misses P/Q and unread columns is confirmed.
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

   **Outcome (2026-10-13, group X, `tests/v3/test_x_lifecycle.sh`, 35/35 on h2dev, ~5 min):** X1/X2 umount and `umount -f` during a resilver, X3 during a scrub (plain umount is refused with EBUSY and the job finishes; `umount -f` kills the client, waits up to 12.5 s for it, then sets `raid_abort`; array mounts, a stopped resilver leaves the disk FAILED, `raid replace` redoes it, scrub clean); X4 fill to 175 MB free, delete, two bulkfree passes, write again; X5 `raid6_simd` encode/decode cross-check both ways plus identical on-disk bytes (the sysctl exists only on arm64, so on h2dev both settings run the C loops: the NEON run is for the Pi); X6 reports the disk count (h2dev has vbd1..4 only: `NDISKS=6` needs `NDISKS=6` in the harness launch and two more 4 GB qcow2 images). **Not covered, a real bug:** a true ENOSPC (X4b, opt-in `X_TESTS=x4b`). When the freemap runs out, `hammer2_xop_strategy_write` completes the bio with `B_ERROR`, and `bpdone()` re-dirties a failed write (`bdirty()` when `B_ERROR` without `B_INVAL`), so the buffer is written again forever ("xop_strategy_write: error 28" at hundreds per second) and the writer blocks in `write(2)` instead of getting ENOSPC. `ps` hangs, the guest needs a reset. Needs a free-space check in the write path (or `B_INVAL` plus a recorded error on ENOSPC). **Fixed (2026-10-13, fix/enospc), X4b is now a normal test in the default X run (about 2.5 min).** Root cause, two parts. (1) `hammer2_vfs_enospace()`, the write-path free-space check inherited from upstream (`hammer2_vop_write` returns ENOSPC at level 2), compared the 5% reserve with `voldata.allocator_free`, which on v3 is the metadata zone over the raw size of every disk and never gets low; the data rows were not counted, so nothing stopped a writer until the stripe bitmap was full. It now uses `hammer2_raid6_space_split()` on v3: free data rows against 5% of the data space (2.5% for root, as upstream), and a metadata zone under its own 5% reads as full, so a flush can always allocate. (2) `hammer2_raid6_stripe_alloc()` returns an errno, and `hammer2_freemap_alloc()` passed that 28 up as a `HAMMER2_ERROR_*` code (28 decodes as bits INCOMPLETE|DEPTH|BADBREF, not ENOSPC). It now maps to `HAMMER2_ERROR_ENOSPC`, and `hammer2_xop_strategy_write` completes such a bio with `B_ERROR|B_INVAL`, `b_error = ENOSPC`: without `B_INVAL`, `bpdone()` calls `bdirty()` and the same write is tried forever (the diagnosis above is right). That is the backstop; data of a write that passed the check but cannot be flushed is dropped with a console message. Not covered: dirty buffers not yet allocated are not counted by the check (as upstream), so the reserve (2.5% to 5%) has to absorb them. X4b: cp gets ENOSPC at 59 x 64 MB of 3960 MB (158 MB left), sync returns, no flush error, cold remount verifies, delete + 2 bulkfree + 28 x 64 MB again, scrub clean.
6. **Seen once, not reproduced (2026-10-11):** page fault in `hammer2_characterize_failed_chain+0xd6` (deref of `chain->dio->bp`) from `hammer2_chain_testcheck`, during group B on h2dev, a degraded 3-disk mount with a concurrent read-path self-heal repair write. The function is marked "not really MPSAFE". vmcore.9 had no module symbols, so the line was not confirmed. Group B passed 13/13 in the two reruns. Suspect: the repair path releases or replaces `chain->dio` while the CHECK FAIL report reads it. Build the module with debug info for h2dev runs so the next dump is usable. **2026-10-12 (516f0fb):** no such interleaving found (testcheck runs with the chain locked and IOINPROG owned; the repair thread never touches `chain->dio`). Hardened anyway: the freemap CHECK FAIL branch dereferenced `dio->bp` without a NULL check (a real fault on a bufferless degraded DIO), now shared NULL-safe `hammer2_chain_print_dio`; the parent walk takes refs under `core.spin`. `deploy.sh` `H2_DEBUG=1` builds with `-g`; `tests/v3/stress_failchain.sh` (not in run_all) ran 53 rounds clean. Not reproduced.
7. **Seen during R8 work (2026-10-11); no longer reproduces (2026-10-12, L6 in 1b9b99e):** when a mount is refused after the super-root fails its check, the unwind can panic in `hammer2_pfsfree_scan` → `hammer2_inode_drop` ("page fault ... panic with 1 spinlocks held"). It was triggered by L4 before the `TESTEDGOOD` fix in ccdda50, and probably affects any mount refused at that stage. Repro: corrupt the super-root on every copy and mount; the mount must fail with an error, not a panic. L6 does this (refused at `Check Error reading super-root`, kmsg-asserted), remounts the corrupt array, also refuses `@NOSUCHPFS` on a healthy one, then mounts fresh: clean on h2dev. The stale `TESTEDGOOD` flag fixed in ccdda50 most likely let the mount get further before failing (inferred, not reproduced). **Rest of the unwinds tested (2026-10-13, L7).** `vfs.hammer2.inject_mount_fail=N` (root only, fires once) fails point N of the first mount of a device: 1 init_devvp, 2 open_devvp, 3 init_volumes (after it succeeded), 4 sm_init before its allocations, 5 sm_init after them, 6 space map rebuild (RW refused EROFS; RO mount is allowed and tested), 7 super-root lookup, 8 super-root check, 9 PFS label lookup, 10 PFS acquire. Not injected: the `!hmp->devvp` check (same exit as 3), `PFS already mounted` (tested for real by mounting a mounted PFS twice, hmp still in use), and the label/copyin EINVALs before any state exists. L7 runs each point 3 times: mount refused at that point, then the same mount succeeds and the data verifies; HAMMER2-mount objects and vnodes do not grow, members can be written raw and newfs'ed afterwards, and no "left hanging"/"left over" messages. Result: all unwinds were already correct (no panic, leak, double free or open device found), so no fix was needed; the code review of the exits agrees (hmp is registered and devvpl moved into it before sm_init, so every later exit goes through `hammer2_unmount_helper`, whose sub-steps tolerate not-yet-started iocom/bulkfree/repair thread/open rows). The only oddity, unrelated to mounts: the HAMMER2-mount object count steps up by 6 to 12 now and then in plain mount + file read + `dmesg -c` cycles with no refusal (80 cycles: 25 -> 48), while 120 bare mount/umount cycles stay flat (26 -> 26). Not chased (probably read or sha256 side, maybe lazily freed); L7 therefore judges a leak by repeated growth over the 30 refusals (at most 5 steps), not by the end count.

---

## 8. Docs and hygiene

Status as of 2026-10-13 (each bullet checked against the tree):

- **`hammer2_raid6.patch` lags `src/`**: it lacks `035b1b7` and today's fix. `install_raid6.sh` uses the patch, `deploy.sh` uses the overlay, and no script regenerates the patch. Add `bin/mkpatch`, and have `deploy.sh` warn when `src/` is newer than the patch. **Done** (850818f: `bin/mkpatch`, `deploy.sh` `check_patch_stale`). The patch itself was last regenerated at 850818f and lags `src/` again (about 30 commits); run `bin/mkpatch` before it is used.
- **`docs/outstanding.md`** is from May (44/44, "I/O timeouts not implemented", "blocked on hardware"). Rewrite it as the current gap list, which this file can seed. **Done** (89be3cd; updated again 2026-10-13).
- **`DEVELOPER.md`** has several stale claims. **Done**: all five below fixed (5a78818), and on 2026-10-13 it was brought in line with layout 3, the space map, the resume cursor, scrub and hung-disk handling:
  - DIRENT is listed as mirrored; it is striped. **Done.**
  - P/Q writes are described as synchronous `bwrite`; they are parallel `bwrite_start`/`wait`. **Done.**
  - `seal_all` is said to seal every row; partial seals remain open. **Done.**
  - It names `test_raid6.c`; the file is `h2raid6_test.c`, and `deploy.sh` doesn't build `h2stripe_check`, so F2 fails on a fresh guest. **Done.**
  - The test table stops at K. **Done** (covers A-Z, marks R as opt-in).
- **`newplan.md §5.4/§5.6`** still describe one slot per write and "no P/Q ever overwritten". Packing and re-sealing changed both; see R4. `io.c:445` cites §5.4 as the reason no pre-read is needed. **Done**: `newplan.md` carries a HISTORICAL header note naming §5.4/§5.6 as superseded, and the `io.c` comment no longer cites it (it now says why a whole-buffer NEW/NEWNZ needs no pre-read).
- **`tests/v3`:**
  - `run_all.sh:79` omits N from the usage text; **Done** (usage text now lists the whole default list, A..Z, and says R is opt-in);
  - temp files are still named `v4_*`; **Done** (`test_v_subcol_alloc.sh` uses `vr_*`);
  - the K2 comment cites a nonexistent "open_rows cap = 16". **Done** (no such text remains).
- **Remove:** **Done**, except the two items marked *kept*:
  - `hammer2_raid6_patch.sh`: removed (6494a87);
  - `fresh_disks.sh` (vn images, unsupported): removed;
  - `tests/other/` (17 of 18 scripts use vnconfig): removed;
  - the obsolete v1/v2 tools in `src/diag` (`h2parity_fix`, `h2diag*`, `diag_map`): removed;
  - `hammer2_raid6.patch.backup`: **kept** (untracked file in the main checkout; deleting it is not approved yet);
  - the 3 MB session JSON (`session-2026-02-26T03-52-3a68a4e6.json`): **kept**, same reason.

  Also fix the "format version 4" string in `h2stripe_check.c`. **Done** (6494a87).

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
  (`stripe_alloc` at `ondisk.c:2738` takes a whole slot or column per chain). LZ4 is the newfs
  default (`HAMMER2_COMP_DEFAULT`), so v3 pays the CPU for it and gets nothing back.
- **Data chain sizes are powers of two, and nothing lies between 32 and 64 KB.** A compressed
  block is stored only if LZ4 fits it in `pblksize / 2` and is rounded up to 1, 2, 4, 8, 16 or 32 KB
  (`strategy.c:970-1039`); otherwise it is stored whole at 64 KB. A file tail is rounded up to a
  power of two by `hammer2_calc_physical`; files <= 512 bytes live in the inode. So a threshold of
  "anything under 64 KB" is the same as 32 KB.
- **Measured on the Pi (Cortex-A72, one core, 64 KB blocks, 2026-10-12):** LZ4 compresses C headers
  and text at ~115 MB/s and decompresses at ~420 MB/s; incompressible data is rejected at
  ~800-1000 MB/s. Stored sizes of 241 text blocks: 1 KB 6, 4 KB 19, 8 KB 103, 16 KB 1, **32 KB 105**,
  64 KB 7. A 16 KB threshold would pack 129 of them, 32 KB 234. Of 30 blocks of the kernel binary,
  25 do not halve and stay 64 KB (ZFS keeps a block that saves 1/8; HAMMER2 needs 1/2).
  LZ4 1.10 is within 2-9% of the 2013 code in the tree (kernel flags, `-mgeneral-regs-only`), and
  LZ4 has no SIMD path to gain from NEON, so the compressor stays as it is.
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
(default 32768, decided 2026-10-12, see 11.6; 0 = off, which is today's behaviour) takes the next
range of the *open sub-column*, aligned to the chain's own size; if none fits it takes a new column
(as now) and opens a sub-column in it. **Size-aligned placement:** every data chain is a power of
two (11.1), so the open sub-column keeps a 64-bit occupancy mask (one bit per 1 KB) instead of a
bump cursor, and a chain takes the first free range aligned to its size (a buddy layout without a
free list; the scan is at most 64 / size positions). A 32 KB chain lands at 0 or 32 KB; a 4 KB
chain followed by a 32 KB one puts the 32 KB at 32 and leaves 4-32 KB for later small chains,
instead of closing the column with 28 KB unused. The column closes when a chain finds no aligned
range (the new chain opens the next column), when it is full, or at gen advance. Filling a gap is
safe: the column is still open and pinned, nothing has been written, so it is not RMW. Once closed,
holes are never reused (11.6 Q5).
`data_off = phys_off + off | radix` (`bref_slot` unchanged). A sub-column is
`{row, disk, gen, used_mask, npending, dio}`; there is one per (current gen, data disk) at most
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

**Rows.** A row still holds up to `ndata` columns, now of up to 64 chains each (1 KB each); `row_complete`,
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
3. **Open sub-column + pin + close in `seal_gen`**, with `npending`, the timeout and size-aligned
   placement (`used_mask`). Default still 0. Tests: J, R (rewrites across sub-columns), F, I and
   `crash_host.sh` (kill with an open column); a placement unit test of mixed 1-32 KB sizes (no
   overlap, alignment, gap fill).
4. **Turn on at 32 KB.** P2 asserts: write amp <= 8x, balance as now, space <= 2x user, P2c
   columns <= 1/3. New P1c: P1 with compressible data (text, ~2x LZ4) must reserve <= 0.6x the
   columns of P1 and keep >= 90% of P1's throughput (the whole stream now goes through the pin). New group Q: 4000 files, remove half, bulkfree x2, remount, every file verified;
   two-disk failure with reads; corrupt one sub-block, scrub finds and repairs exactly it; overwrite
   in the same TXG (pending and closed). N and T (reuse) for capacity.
   **Done 2026-10-12 (b6f2894, d1fa016, 993b683; merged b8cf3b8).** h2dev: P2 write amp 35x -> 4.2x,
   columns 4000 -> 251, df 16x -> 1.44x user; P2c 63 columns, 2.96x; P1c 2048 columns (0.5x P1), 1.03x.
   Balance is judged on data columns per disk (63/61/63/64), bytes only printed. P1c throughput on the VM
   is above P1 (half the bytes, LZ4 cost hidden); the 90% target still has to be checked on the Pi.
   Q5 churn (4 rounds x 1000 files): 374 columns allocated, 300 live at 25% mean occupancy, so step 5
   matters. Pi: L K D V passed with packing on (kernel #30, sysctl) before the default changed.
5. **Bulkfree per column** (`bf_mark`/`bf_end`), still with the v1 map (refcount byte becomes a
   bitmask of 8 data disks, or an in-memory-only mask). Test: Q after rm leaves ~50% of columns freed;
   Q churn variant (create, delete half, repeat) reports dead columns (11.6 Q5).
   **Step 5 design and result (2026-10-12).** Checked first: today's bulkfree is per row (slot):
   `bf_mark` sets one bit per slot, `bf_end` frees a row only when no chain in any of its columns
   was found in two consecutive passes. So a shared column was never freed early (no data-loss
   bug; Q1 removes every other file and checks survivors, and now also that no live column is lost
   and `rz_bf_reclaimed` stays 0). What was missing was the per-column picture. Choice: the
   **in-memory-only mask**. `stripe_bf_seen` is now a byte per slot, `bf_mark` ORs
   `1 << (copyid & 7)` (the chain's disk), `bf_end` counts live columns (popcount) and dead ones
   (`rows referenced x ndata - live`; a row sealed short counts its unused columns as dead, they
   are as unusable). The mask is rebuilt by every scan and never persisted, so a crash cannot
   leave it stale (the worst a crash does is lose the staged set, as before), the v1 refcount byte
   keeps its meaning and an older kernel mounts the array unchanged. Over 8 disks bits alias,
   which only over-counts live columns and cannot make a live row look dead. Why not the on-disk
   bitmask: a column cannot be reused whatever its bit says (sealed P/Q cover it, a dead column
   would need RMW, 11.6), so the row stays the unit of freeing and the persisted mask buys
   nothing until resilver/scrub skip dead columns (step 6). Consequence for the test: "~50% of
   columns freed" holds when a whole batch dies (Q6: two batches in separate TXGs, one removed, live
   columns 40-60% of before). Removing every other file frees nothing, since every column keeps a
   live chain (Q1, status quo by 11.6). Counters: `vfs.hammer2.rz_bf_{rows_freed,reclaimed,
   rows_live,cols_live,cols_dead}`; Q5 now prints them per round.
   **Result (h2dev, 4 disks):** Q6 369 -> 185 live columns, 93 rows freed; Q1 (every other file)
   374 -> 361, the 13 that died are columns of only 32 KB (odd) files; Q5 churn, 370 columns
   allocated, 290 live, 54 dead in held rows, 14 rows freed (was: nothing reported). The dead
   columns cannot be reused, only reported; reclaiming them needs repack, left out (11.6).
   **P2 on the Pi: 11.7x vs 4.2x (investigated 2026-10-13, h2dev; no fix).** New counters
   `vfs.hammer2.p10_{mf_sync,mf_maxpend,mf_blocks,mf_defers,dc_trigger,dc_stall}` (TXG flush
   passes, MIRROR_MAXPEND early flushes, mirror blocks written, copies queued, dirty-chain
   syncer triggers and stalls); P2/P2c print them as an INFO line. Cause: **more flush passes**,
   driven by the dirty-chain limit (`maxvnodes/10`: 10794 on h2dev, 4710 on the Pi), not
   MAXPEND (`mf_maxpend` is 0 in every run: only ~119 distinct metadata blocks are ever queued,
   256 is never reached). Every pass COWs and writes again the directory indirect blocks and the
   freemap leaf (about 56 blocks, constant) plus the inode blocks touched since the last pass,
   to disk 0 and N-1 mirrors, about 0.4-0.5x of the 15 MB user bytes per extra pass. h2dev,
   P2 write amp / passes / mirror blocks: limit 10794 (default) 4.8-5.7x / 4 / 122-193 (run to run); 4710 5.5x / 5 /
   207; 3000 6.2x / 6 / 263; 1500 9.2x / 10 / 470; 800 8.1x / 22 / 304. A sync every 1-6 s during
   the cp gave only 4-6 passes (the cp is a few seconds on the VM), 4.2-5.5x. The Pi's cp takes
   about 30 s on slow USB, so the syncer, triggered at limit/2 = 2355 chains, runs back to back
   (expected ~10 passes, which matches the 1500-limit run). The passes are COW-inherent and each
   one must mirror what it commits (R1), so there is no clean way to make a pass cheaper;
   throttling the limit-triggered syncer (a minimum pass interval) would trade memory bounds for
   fewer passes and was not done. Recommendation: keep the 8x assert on h2dev, check
   `mf_sync` on the Pi (expect about 10), and if it is the limit, allow ~13x for P2 on machines
   whose `limit_dirty_chains` is below ~5000 (or raise the limit for 2 GB machines after
   measuring chain memory); P2c is 4-6x there.
   **Pi measured (k35, 2026-10-13):** `mf_sync` +6, `mf_maxpend` 0, `mf_blocks` +422, `dc_trigger`
   +447, write amp 8.53x (P2c 7.10x, 6 passes, 444 blocks). So 6 passes, not 10, but about 70
   mirror blocks per pass against about 41 on h2dev at the same limit (5 passes, 207): the slow cp
   leaves more inode blocks dirty per pass. Over k32-k35 the Pi gave 11.7, 8.2, 8.6, 8.5x.
   **Decided:** P2/P2c allow 12x when `limit_dirty_chains` < 5000, 8x otherwise (test_p).
   **Next lever (done 2026-10-13, see section 12):** metadata goes to the primary plus N-1 mirrors, 4 copies on
   4 disks, where RAID6 needs 3 to survive any 2 failures. 3 copies (2 mirrors) cut mirror
   writes by 1/3 on 4 disks and by 1/2 on 6 (Pi P2 about 6.7x: 422 x 64 KB x 3 = 83 MB of mirrors, 28 MB less); it needs a placement rule for
   the two mirror disks used by write, failover read, resilver and scrub, and arrays that
   already have all-disk mirrors stay readable (the extra copies are just ignored).
6. **Later: sm v2 / S1 mask** and resilver/scrub skip of dead columns (P6); S3 fold-in.

### 11.6 Open questions (all decided 2026-10-12)

- ~~Threshold: 16 KB or 32 KB?~~ **Decided 2026-10-12: 32 KB** (all sub-64 KB chains; 11.1 shows
  the two are the same, since no chain lies between 32 and 64 KB). At 16 KB a 64 KB block that LZ4
  halves (the common 2-3x case lands in the 32 KB bucket: 105 of 241 text blocks on the Pi) still
  takes a whole column, so compression would keep saving nothing. Pinned memory is the same for
  both. The cost is that compressible streams go through the pin (P1c) and 32 KB chains need the
  size-aligned placement of 11.4.
- ~~fsync cost?~~ **Decided 2026-10-12: as today for P5.** An `fsync` of a tiny file may cost a
  full column plus P/Q (>= 192 KB on 4+2). Writing only the touched 4 KB is ruled out for good: it
  needs RMW parity and returns the write hole. If fsync-heavy workloads matter later, the tool is an
  intent log (a mirrored small log, like the ZFS ZIL), as a separate project.
- ~~Option and mirror width for (b1)?~~ **Decided 2026-10-12: option (a)**, packing chains into a
  shared column, so the (b1) mirror width does not arise. If (a)'s pin proves unworkable, (b1) with
  3 copies (survives 2 failures, as RAID6) is the fallback, not the all-disk mirror.
- ~~Must v1 maps stay mountable by an older kernel?~~ **Answered from the code:** `sm_read` accepts a
  header only if `version == HAMMER2_SM_VERSION` (`ondisk.c:1767`); anything else takes the "no
  valid space map header" path and is rebuilt by tree walk. An older kernel therefore mounts a v2
  array (slower mount), writes v1, and the newer kernel converts back. No upgrade gate is needed.
  Condition for step 6: the reserved space map area must fit the 4x larger v2 map.
- ~~Hole reuse / repack?~~ **Decided 2026-10-12: left out.** Holes in a closed column are never
  reused and there is no repack pass; the worst case is a column pinned by one live chain (status
  quo). Group Q gets a churn variant (create, delete half, repeat) to measure dead columns; a repack
  design is revisited only if they pile up.

## 12. Metadata on 3 copies (2026-10-13)

**Problem.** A v3 metadata block was written to its primary (the dio's disk, always disk 0, P4)
and to every other member, N copies. RAID6 only has to survive two failures, so three copies on
three distinct disks are enough, and the other N-3 writes are pure cost (P10, section 11.5 "Next lever").

**Placement rule** (`hammer2_md_copy_disk()` and friends in `local_hammer2_raid6.h`, the one place
that knows it). For a block at per-disk offset `off`, `b = off >> 16`, `n1 = ndisks - 1`, primary `p`
(the disk the dio is keyed on):

| copy | disk |
|---|---|
| 0 | `p` |
| 1 | `p + 1 + (b % n1)` mod ndisks |
| 2 | `p + 1 + ((b % n1) + 1 + ((b / n1) % (n1 - 1))) % n1` mod ndisks |

Copy 2 is `1 .. n1-1` places after copy 1 among the other disks, so the three are distinct for
any ndisks >= 4 (n1 >= 3, so `n1 - 1 >= 2`). Checked by a host program over 100000 blocks for
N = 4..8: no collision, every mirror disk gets the same number of copies (4 disks: 66667/66666/66667),
and all ordered mirror pairs occur (N = 4: 16666-16667 blocks each). So mirror load is spread over
disks 1..N-1 and any 3-subset of 4 disks, minus the primary, is used; any 2 failed disks leave a copy
(on 4 disks every 3-set misses just one disk). `hammer2_md_ncopies(ndisks, layout)` is
`HAMMER2_MD_COPIES` (3) for layout 3 with more than 3 disks, else ndisks: with ndisks <= 3
(unsupported) or on a layout 2 array the rule degenerates to "every disk", copy k = `p + k`. The count is
`hmp->md_ncopies`, set at mount.

**Consumers** (each now iterates the copy set, not all disks):
- `hammer2_io_metadata_mirror_write` (the deferred queue of P10 is unchanged, it queues the block once
  per TXG and the flush calls this with the block's primary; a down primary is skipped, a rebuilding
  member among the copies still gets the copy, unwaited under `rebuild_lk`).
- `hammer2_io_metadata_read_disk` (cold read spread): copy `b % ncopies` of the block's copies;
  `meta_spread_read` passes the dio's primary.
- `hammer2_io_metadata_mirror_read` (failed primary): the other copies, starting at that same copy,
  two passes (the second waits for busy buffers) as before.
- `hammer2_io_metadata_mirror_heal` (check-failed block): candidates are the block's copies only.
  A disk without a copy would hold zeros or an older block of the same offset; the CHECK would reject it
  but it is wasted I/O and, for an old kernel's mirrors, a silent-wrong-source risk.
- Resilver Phase A: copies a chunk only if the target holds a copy of it (the metadata extents and
  the reserved zone segments both), so a non-primary disk gets about 2/(N-1) of the zone (4 disks:
  two thirds; X8b measured 69.9 of 100.7 MB with `resilver_skip_unalloc=0`, the extra over 2/3 is the zone
  segments). The source is any other copy that is not the primary (the primary's newest copy may be a
  delayed write), then the serial fallback over the copies (`hammer2_resilver_md_read`).
  The P6 resume cursor is about stripe slots (Phase 3) and Phase A reruns whole on resume, so it is
  unaffected.
- Scrub: the tree-walk checks DATA and DIRENT brefs; metadata is checked when the walk loads the chains,
  through the read path above, so nothing to change. A bad copy that is never read is not found by it
  (same as before, and the same as a mirror on a disk that was never read in the all-disk scheme).
- Degraded writes: a copy on a failed, non-rebuilding disk is not written; the block keeps its other
  copies (two after one failure, one after two). A block written while mirror disk D was down has
  no copy on D; the later resilver of D copies only blocks D is designated for, from a copy that
  exists (the write path and the resilver apply the same rule, and D was in the block's set at write
  time or it was not). Tests Z4, Z5.
- Replace is the resilver. fsck/diag tools: `h2stripe_check` only needed the layout number.

**Compatibility.** An older kernel (layout 2) assumes a mirror on every disk: its cold read spread
`b % ndisks` may pick a disk that has no copy and take it unchecked as the dio's buffer, its resilver
Phase A reads "any survivor" and would write a never-written or stale block to the new disk, and its
failover tries siblings in index order. The CHECK catches most of the first, nothing catches the
second. So an array written with 3 copies must not mount on that kernel: `rz_layout` goes 2 -> 3
(`HAMMER2_RZ_LAYOUT`), which the old `hammer2_raid6_sm_init` already refuses with "stripe layout 3,
this kernel needs 2" and EINVAL (clean, nothing written). Downgrade is therefore not supported once
a layout 3 array is written (it would be cheap only by keeping layout 2 and so never converting);
re-making the array is the way back.

The new kernel accepts layout 2 and 3 (`HAMMER2_RZ_LAYOUT_MIN` = 2):
- **layout 2 array** (made before): `md_ncopies = ndisks`; it keeps writing all-disk mirrors, an older
  kernel still mounts it, nothing changes. Pure compatibility.
- **conversion** is explicit: `sysctl vfs.hammer2.md_3copy_upgrade=1` at a read-write mount sets
  layout 3 in the in-memory and sync volume headers (written by the first flush) and logs
  "metadata converted to 3 copies". Blocks written earlier stay on every disk (the extra copies are
  simply never read: they lie on disks the rule does not name, the rule names only disks that have one);
  later ones get 3. Irreversible for older kernels. Group Z6 tests both.
- **new arrays** (`newfs_hammer2 -R 6`) are layout 3. `HAMMER2_MKFS_LAYOUT2=1` in the environment makes
  newfs write layout 2 (test knob: also the baseline for the P2 comparison).

**Risks.** (1) Only 2 copies after one disk failure and 1 after two: a bitrot hit in that window on
the last copy is unrecoverable, as for the data columns (RAID6 with two failed has no redundancy
either). All-disk mirrors would have had N-2 left. (2) Heal when the cold read came from a mirror
(`alt_disk`) skips that mirror and tries the primary's media copy (the dio holds the primary's buffer,
so `read_sibling`'s non-blocking `getblk` fails and it reads through a pbuf; a free cached buffer
holds the bad bytes and simply fails the check), then the other mirror. A good primary only repairs
the mirror. (3) Distribution: disk 0 holds the primary of every block (`bdwrite`, coalesced),
mirrors are spread: on 4 disks disk 0 now writes less than the others (P2 per-disk deviation rose
from ~0.13-0.3 to ~0.3-0.45, informational only).

**Tests.** Group Z (`test_z_meta3copy.sh`):
- Z1: raw media: every metadata block disk 0 holds is on exactly 3 disks, mirrors on disks 1..3.
- Z2: all 6 pairs failed (their metadata and data zones wiped): mount, read 302 files cold, scrub clean.
- Z3: the primary and one mirror of every metadata block corrupt (random 64 KB): reads, scrub, no unrepairable.
- Z4: each disk position resilvered (with files written while it was down), then the next two disks
  failed and wiped: reads and scrub.
- Z5: mirror disk 1 fails during writes, then disk 0 (the primary) or 2 fails: reads, scrub.
- Z6: layout 2 array mounts, keeps N copies, reads, survives disks 1+2 failed; conversion sysctl,
  3-copy writes, old and new files survive disks 0+3 failed.
- X8b changed: with the target a copy holder only for 2/(N-1) of the zone, `skip_unalloc=0` copies that, not the zone.
- R/F: `h2stripe_check` prefers `/root/h2diag` (deploy builds it from this tree) over a stale `/usr/local/bin` copy.

**Results (h2dev, 4 disks, 2026-10-13).** A B C D G K L M P R X Y Z all pass (F 4/4, R 18/18, X 83/83,
M 5/5, Y 26/26, Z 42/42). P2 write amplification, same build, layout 3 vs `HAMMER2_MKFS_LAYOUT2=1`
(five runs each, noisy through the flush pass count `mf_sync` 3-5 and `mf_blocks` 113-215):
layout 2 (all-disk) 5.85 4.35 5.77 4.73 5.49, mean 5.24x (mean 81 MB written, 174 mirror blocks);
layout 3 4.08 4.66 4.26 5.06 4.60, mean 4.53x (mean 70 MB, 173 blocks). The saving, 11 MB, is one third
of the 33 MB the mirrors cost (174 blocks x 64 KB x 3), as the model says (base cost, everything but
the mirrors, is 47-50 MB in every run). On the Pi (4 disks, 422 blocks x 64 KB) that is 79 MB -> 53 MB,
26 MB less: P2 8.5x -> about 6.9x, to be measured. A 6-disk array would save half of the mirror cost.
