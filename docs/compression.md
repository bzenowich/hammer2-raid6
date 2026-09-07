# HAMMER2 v3 — Optional LZ4 Compression Plan

**Status**: Draft. Pre-implementation.
**Scope**: Enable LZ4 compression as an opt-in inode attribute on both
plain HAMMER2 mounts and v3 RAIDZ2-native (raid6) mounts.
**Cross-refs**: `DEVELOPER.md` (I/O paths, open-row), `newplan.md` §5,
`stripe_bitmap.md`, `zfs_compare.md` (M2 gap).

---

## 1. Starting point — what already exists

Upstream HAMMER2 already ships LZ4 + ZLIB compression. Code is in-tree
and live for plain (non-raid6) mounts:

| Piece | Location |
|-------|----------|
| Algo IDs | `local_hammer2_disk.h` — `HAMMER2_COMP_{NONE,AUTOZERO,LZ4,ZLIB}` |
| Inode field | `hammer2_inode_meta.comp_algo` (per-inode) |
| Bref methods | `HAMMER2_ENC_COMP(algo)` packed into `bref.methods` |
| Write path | `hammer2_compress_and_write()` in `local_hammer2_strategy.c` |
| Read path | `hammer2_decompress_LZ4_callback()` in `local_hammer2_strategy.c` |
| Mkfs default | `HAMMER2_COMP_NEWFS_DEFAULT = HAMMER2_COMP_LZ4` (plain), `AUTOZERO` for special inodes |
| Userspace | `mkfs_hammer2`, `hammer2 setcomp` (upstream) |

**Implication**: this is not "add compression." Plain-mount LZ4 is
assumed to work as upstream behaves. The job is:

1. Verify plain-mount LZ4 still works in our fork (no regression from v3 changes).
2. Make LZ4 work correctly on raid6 mounts where the stripe allocator,
   open-row machinery, stripe bitmap, scrub, and resilver all live below
   the compression layer.
3. Wire opt-in controls and document the operator UX.

---

## 2. Design constraints from v3 RAIDZ2

Compression happens **above** the raid6 stripe allocator. The write
path is:

```
buf → compress (LZ4) → round to radix → allocate stripe slot
    → write data column(s) → update parity → flush open-row → seal
```

Key interactions:

- **Variable bref.bytes**: compressed blocks shrink to a smaller radix
  (e.g. 64 KB → 16 KB). Stripe allocator already handles variable
  widths in v3 (`hammer2_raid6_stripe_alloc` in `local_hammer2_chain.c:1600`),
  but every test to date has used 64 KB full blocks. Sub-PBUFSIZE
  allocations must land correctly in open rows.
- **Open-row packing**: an open row currently fills with 64 KB units
  per column. Compressed blocks of 4/8/16/32 KB must coexist within the
  same column without splitting parity coverage. Need to confirm the
  per-column write granularity is the radix-rounded compressed size,
  not PBUFSIZE.
- **Stripe bitmap granularity**: `stripe_bitmap.md` tracks
  per-(stripe,column) allocation. If a compressed block is <64 KB, the
  column slot is still consumed as one allocation unit, with the unused
  tail wasted. **Decision**: accept this waste in v1 (matches upstream
  HAMMER2 freemap allocation behavior). Sub-column packing is a v2
  optimization.
- **Checksum domain**: XXH64 is computed over the **compressed**
  bytes (current upstream behavior). Scrub and resilver verify
  checksum without decompressing. No change needed.
- **Parity domain**: P+Q are computed over the **on-disk bytes** (i.e.
  compressed + radix pad). Decompression happens only at the VFS read
  layer, well after parity reconstruction. No raid6 code touches
  compressed payload semantically — it sees opaque bytes.
- **Read-reconstruct path**: when parity rebuilds a missing column, the
  reconstructed bytes feed into `hammer2_decompress_LZ4_callback` the
  same way a direct read would. Verify the bio chain still passes
  `bref.methods` through reconstruct → decompress.

---

## 3. Work items

### 3.1 Verify plain-mount LZ4 (no regression)

- Mount plain HAMMER2 image, write known-compressible data, unmount,
  remount, read back, verify bytes + `hammer2 stat` reports
  `comp_algo=LZ4`.
- Run existing harness compressible/incompressible mix.
- Confirm `bref.methods` decoded `HAMMER2_DEC_ALGO` returns LZ4 on
  written blocks.

### 3.2 raid6 write path — sub-PBUFSIZE allocation

- Audit `hammer2_raid6_stripe_alloc` for assumptions that `bytes ==
  HAMMER2_PBUFSIZE`. Anything that hard-codes 64 KB needs to use
  `chain->bytes` (radix-rounded compressed size).
- Open-row `add_data` must accept a column write of arbitrary
  radix-aligned size ≤ PBUFSIZE.
- Parity computation in `hammer2_raid6_gen_syndrome` operates on a
  full column (64 KB). Compressed sub-column writes pad to 64 KB for
  parity input. Confirm padding is **zero-fill** and deterministic
  across reads (otherwise scrub will see parity mismatch).
- Decision point: do we compute parity over the radix-rounded size or
  over the full 64 KB column? **Recommendation**: full column with
  zero pad. Simpler invariant for scrub; cost is parity CPU only, no
  extra disk I/O.

### 3.3 raid6 read path — decompress after reconstruct

- Confirm `hammer2_strategy_read` bio callback chain delivers the
  `bref` (with `methods`) to `hammer2_decompress_LZ4_callback` after
  any P/Q reconstruction.
- Test degraded read (1 disk failed, 2 disks failed) with compressed
  inode. Bytes-out must equal pre-compression input.

### 3.4 Scrub interaction

- Scrub re-reads each stripe and verifies XXH64 over compressed
  payload + recomputes P/Q over zero-padded column. No decompression
  needed.
- Ensure scrub does NOT attempt to decompress for verification (would
  waste CPU and confuse the check_algo invariant).
- Test: corrupt 1 byte in a compressed block on disk, run scrub,
  confirm detection + repair from parity.

### 3.5 Resilver interaction

- Resilver rebuilds a replaced disk column-by-column from the
  remaining data + parity. Compressed blocks are opaque bytes; no
  special handling.
- Test: degrade, replace, resilver an array holding compressed inodes,
  read back post-resilver, verify decompression succeeds.

### 3.6 Operator UX

- `mkfs_hammer2 -C lz4` (or default-on) to set root inode comp_algo.
- `hammer2 setcomp lz4 <path>` for per-subtree opt-in/opt-out.
- `hammer2 stat` to surface comp_algo + on-disk compressed size +
  ratio.
- Mount option `comp=lz4|none` to override default for new files (does
  not rewrite existing). Inherits to children at creation time.

### 3.7 mkfs default policy on raid6

Open question: should `mkfs_hammer2 --raid6` default `comp_algo` to
LZ4 like the plain mkfs does?

- **Pro**: free space savings, LZ4 is cheap, matches upstream default.
- **Con**: more variables during raid6 bring-up bug hunts; harder to
  reason about parity invariants when block sizes vary.
- **Recommendation**: default `NONE` on raid6 until §3.2–3.5 land and
  the test matrix is green. Flip default after stabilization.

---

## 4. On-disk format impact

**None**. All required fields exist:

- `hammer2_inode_meta.comp_algo` — already present, 1 byte.
- `bref.methods` — already encodes per-block comp algo.
- Stripe bitmap — tracks column slots, agnostic to payload size.
- Refcount zone — counts column allocations, agnostic.

Volume header version does **not** need to bump.

---

## 5. Test matrix

| # | Test | Plain | raid6 |
|---|------|-------|-------|
| T1 | Write compressible (zeros / text), read back equal | ✓ | ✓ |
| T2 | Write incompressible (random), confirm `COMP_NONE` fallback | ✓ | ✓ |
| T3 | Mixed compressible/incompressible workload, scrub clean | ✓ | ✓ |
| T4 | Unmount/remount round-trip | ✓ | ✓ |
| T5 | Corrupt 1 byte in compressed block, scrub detects + repairs | — | ✓ |
| T6 | Degrade 1 disk, read compressed inode, bytes correct | — | ✓ |
| T7 | Degrade 2 disks, read compressed inode, bytes correct | — | ✓ |
| T8 | Resilver after replace, compressed inode survives | — | ✓ |
| T9 | `setcomp` toggle mid-file, new writes use new algo, old blocks unchanged | ✓ | ✓ |
| T10 | High-fanout small-file workload (many sub-radix writes) | ✓ | ✓ |
| T11 | LZ4 expansion-bomb (compressed > original) falls back to NONE | ✓ | ✓ |

Add T5–T8 to `raidz2native_test_spec.md`.

---

## 6. Performance notes (not goals, just expectations)

- LZ4 single-thread ~500 MB/s compress, ~2 GB/s decompress on modern
  x86. On the Phase 3 Ryzen 7 2700, expect ≥1 GB/s per core
  decompress.
- Parity CPU cost dominates over LZ4 cost for raid6 writes. LZ4
  should be in the noise.
- Compressed-block radix waste: typical 4–8 KB per sub-column block.
  On a 4-disk array with ndata=2, every compressed write wastes up to
  2 × (PBUFSIZE − radix(comp_size)) in parity-padded space. Acceptable
  for v1.

---

## 7. Phasing

1. **Phase A** — verify plain-mount LZ4 in fork (no code change
   expected). Land T1–T4, T9–T11 plain results.
2. **Phase B** — raid6 write/read path audit (§3.2, §3.3). Land T1–T4
   raid6 results.
3. **Phase C** — scrub + resilver compat (§3.4, §3.5). Land T5–T8.
4. **Phase D** — operator UX polish (§3.6). Flip raid6 mkfs default if
   matrix is clean.

No new disk format. No new zones. No quorum changes. All risk is in
the v3 stripe allocator interaction with sub-PBUFSIZE writes.

---

## 8. Open questions

- Does the v3 open-row TAILQ tolerate columns of mixed
  radix-rounded sizes within one row? (Probably yes — it tracks per
  column — but needs explicit test.)
- Is XXH64-over-compressed-bytes the right invariant for raid6, or
  should we checksum decompressed bytes to catch decompression bugs?
  Upstream chose compressed; sticking with that keeps scrub
  decompression-free. Revisit only if we see real decomp bugs.
- Should `comp=zstd` (newer, better ratio, similar speed) land in the
  same series? **No** — separate effort, requires new algo ID and
  on-disk format awareness across mounts. Defer.
