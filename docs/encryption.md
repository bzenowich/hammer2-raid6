# HAMMER2 v3 raid6 — Encryption-at-Rest Plan (tcplay)

**Status**: Draft. Pre-implementation.
**Scope**: Operator playbook + integration plan for encrypting v3
raid6 arrays using `tcplay(8)` underneath HAMMER2. No code changes to
HAMMER2 itself.
**Cross-refs**: `DEVELOPER.md`, `newplan.md`, `compression.md`,
`zfs_compare.md` (M2 native-encryption gap), `workflow.md`,
`physical_disk_tasks.md`.

---

## 1. Why under raid6, not above

The encryption layer sits **below** the raid6 stripe layer:

```
HAMMER2 v3 (raid6)
   ↑ plaintext bytes
+--+--+--+--+
|dm|dm|dm|dm|   ← tcplay-mapped /dev/mapper/h2crypt{0..N}
+--+--+--+--+
   ↑ ciphertext
+--+--+--+--+
|d0|d1|d2|d3|   ← raw /dev/da{0..N}
+--+--+--+--+
```

Reasons:

- **No HAMMER2 code change**. raid6 sees plaintext block devices; XXH64
  checksums, P+Q syndromes, open-row machinery, scrub, resilver all
  remain bit-identical to unencrypted operation.
- **Disk replacement is symmetric**. Each disk is independently
  encrypted with the same KEK material. Resilver writes ciphertext to
  the new disk's tcplay mapping — raid6 never sees the cipher
  boundary.
- **Failure isolation**. A corrupted dm-mapping on one disk surfaces
  as I/O errors on that column, which raid6 already tolerates (up to 2
  column losses).
- **Above-raid6 alternative rejected**: encrypting the assembled raid6
  volume means one giant ciphertext blob; loss of the header kills the
  whole array. Per-disk gives N independent recovery paths and
  natural rekey/rotate boundaries.

---

## 2. Components

| Piece | Source | Role |
|-------|--------|------|
| `tcplay(8)` | DragonFly base since 2.11 | VeraCrypt-format volume create/map/unmap |
| `dm(4)` | DragonFly base | Device mapper, target for tcplay mappings |
| `dmsetup(8)` | DragonFly base | Lower-level mapping ops, diagnostics |
| HAMMER2 v3 | this fork | Mounts the assembled raid6 over mapped devices |

No new ports, no new kernel modules.

---

## 3. Key model

### 3.1 One passphrase + one shared keyfile

- Operator passphrase: ≤64 chars, prompted at boot/mount.
- Keyfile: 1 MB random blob stored on a separate medium (USB token,
  TPM-sealed file, ops vault). All N disks bind to the same
  passphrase + keyfile pair.
- Each disk's tcplay header still contains its own salt + master key,
  so headers are not byte-identical, but unlock material is shared.

**Rationale**: simplifies operator UX (one prompt unlocks the whole
array). Cost: losing the keyfile loses every disk simultaneously —
acceptable since losing the keyfile in an above-raid6 design is
identical.

### 3.2 Header backup

`tcplay --save-hdr` per disk → store the N header files in the ops
vault alongside the keyfile. Header is small (~512 bytes). Without it,
a single corrupted disk header is unrecoverable even with the
passphrase.

### 3.3 Cipher choice

Default: **AES-256-XTS** (tcplay default for VeraCrypt mode).
Cascade ciphers (`-b AES-256-XTS,SERPENT-256-XTS,...`) available but
not recommended — 2–3× write CPU for marginal threat-model gain on a
storage server.

### 3.4 PRF

Default: **SHA-512**. Whirlpool/RIPEMD-160 supported. Stick with
SHA-512 unless ops policy dictates otherwise.

---

## 4. Operator workflow

### 4.1 Initial provisioning

```sh
# 1. wipe + create per-disk tcplay volumes (4-disk array example)
for d in da0 da1 da2 da3; do
    tcplay -c -d /dev/$d -a SHA-512 -b AES-256-XTS \
           -k /var/h2crypt/keyfile
done

# 2. map each disk
for i in 0 1 2 3; do
    tcplay -m h2crypt$i -d /dev/da$i -k /var/h2crypt/keyfile
done

# 3. mkfs raid6 across mapped devices
newfs_hammer2 --raid6 \
    /dev/mapper/h2crypt0 /dev/mapper/h2crypt1 \
    /dev/mapper/h2crypt2 /dev/mapper/h2crypt3

# 4. mount
mount_hammer2 /dev/mapper/h2crypt0@LABEL /mnt
```

### 4.2 Boot / remount sequence

`rc.d` script (new — `/etc/rc.d/h2crypt`) before
`/etc/rc.d/mountcritlocal`:

1. Prompt for passphrase (or pull from sealed source).
2. Iterate disks, `tcplay -m h2crypt$i -d /dev/da$i -k <keyfile>`.
3. Wait for all `/dev/mapper/h2crypt*` nodes to settle.
4. `mount_hammer2` proceeds against the mapped devices.

Failure of any single unmap = degraded raid6 boot. Allow boot to
continue with N−1 or N−2 disks unlocked (raid6 tolerates 2 missing
columns); log loudly.

### 4.3 Shutdown

`rc.d` priority opposite of boot. Unmount HAMMER2 → `tcplay -u
h2crypt$i` for each disk. Mapping leakage across reboot is harmless
(dm tables are RAM-only) but unmounting cleanly avoids stale
references in `/dev/mapper`.

### 4.4 Disk replacement (degraded → resilver)

```sh
# physical swap: failed da2 → fresh disk now at da2
tcplay -c -d /dev/da2 -a SHA-512 -b AES-256-XTS -k /var/h2crypt/keyfile
tcplay -m h2crypt2 -d /dev/da2 -k /var/h2crypt/keyfile
hammer2 resilver <raid6 vol> 2 /dev/mapper/h2crypt2
```

raid6 resilver writes plaintext to the new mapping; tcplay encrypts
in-flight; disk holds ciphertext. No special knowledge in the
resilver path.

### 4.5 Rekey / passphrase rotation

`tcplay --modify` rewrites the header with a new passphrase but
preserves the master key — no data rewrite. Do per disk:

```sh
for i in 0 1 2 3; do
    tcplay --modify -d /dev/da$i -k /var/h2crypt/keyfile.old \
           -k /var/h2crypt/keyfile.new
done
```

Full **master key rotation** requires draining the array (replicate
out, mkfs fresh with new keys, replicate back). Document but don't
automate.

---

## 5. Interaction with v3 raid6 features

### 5.1 Volume header quorum

`volhdr_quorum.md`: HAMMER2 reads volume headers from all disks and
picks the quorum winner. With per-disk encryption, each disk must be
mapped *before* HAMMER2 starts reading headers. If one disk fails to
unlock, HAMMER2 sees N−1 headers — still meets quorum for N=4. For
N=3 (minimum array), losing one map = no quorum. Boot script must
treat unmap failure as degraded, not fatal, only when ≥quorum disks
are mapped.

### 5.2 Scrub

Scrub reads ciphertext through dm, gets plaintext, checksums it.
Identical to unencrypted scrub. No code change. Performance: add dm
overhead (~5–10% on AES-NI hardware).

### 5.3 Resilver

See §4.4. Plaintext on the wire, ciphertext on disk. The resilver
path is unchanged.

### 5.4 Stripe bitmap + refcount zones

These zones are *inside* HAMMER2 plaintext space → encrypted
transparently by the dm layer. No special handling.

### 5.5 Compression (see `compression.md`)

Stacking order:

```
HAMMER2 LZ4 (per-block)
   ↓
raid6 stripe (parity over compressed bytes)
   ↓
tcplay (encrypts whatever raid6 writes)
   ↓
disk
```

Compress-then-encrypt is the correct order (encrypted ciphertext is
incompressible). Already the case here because compression lives
above raid6 and encryption below it.

---

## 6. Threat model

**In scope**:

- Disk theft / decommission leak. Drive pulled from chassis reveals
  ciphertext only.
- RMA return of failed disk. Same.
- Cold boot on stolen chassis: passphrase + keyfile not present →
  array unmappable.

**Out of scope** (document, don't solve):

- Live system compromise. Plaintext is in RAM and on `/dev/mapper/*`.
- Evil-maid against the boot partition. tcplay binary integrity
  unprotected without secure boot.
- Side-channel (timing, power) against AES-NI.
- HAMMER2 metadata leakage via I/O patterns (block sizes, access
  cadence). XTS preserves block-position info.

---

## 7. Performance expectations

- AES-NI x86: ~3–5 GB/s per core single-stream encrypt/decrypt.
- Ryzen 7 2700 (Phase 3 box): ≥8 cores w/ AES-NI → ceiling well
  above 4-disk WD Red sequential throughput (~150 MB/s each = 600
  MB/s aggregate).
- Expected overhead: 5–10% on sequential, ~negligible on small
  random (parity CPU dominates).
- Memory: dm-crypt buffers per active request, bounded by dm queue
  depth. Not a concern at this disk count.

---

## 8. Test matrix

| # | Test | Outcome |
|---|------|---------|
| E1 | Create, map, mkfs, write/read, unmap, remap, read | Bytes equal |
| E2 | Reboot loop (10×) with rc.d unlock | All boots mount clean |
| E3 | Wrong passphrase | All `tcplay -m` fail; HAMMER2 mount aborts |
| E4 | Lose keyfile mid-session | Existing mount survives until unmount; remount fails |
| E5 | Header corruption on 1 disk + saved hdr restore | Recovery succeeds |
| E6 | Degraded boot (1 disk fails to unlock, ≥quorum mapped) | HAMMER2 mounts degraded; raid6 resilver-able after fix |
| E7 | Disk replace + tcplay create + resilver | Array returns to clean |
| E8 | Scrub on encrypted array vs unencrypted baseline | Same correctness; perf delta noted |
| E9 | Passphrase rotation per §4.5 | All disks accept new pass; data intact |
| E10 | LZ4 + encryption stack (per §5.5) | Compressed ratios match unencrypted-mount baseline |
| E11 | Yank cable on 2 disks mid-write, recover | raid6 recovers; tcplay headers intact |

Fold E1–E11 into `raidz2native_test_spec.md` as an optional
"encrypted-mount" axis (skipped by default, run on the encrypted-image
harness target).

---

## 9. Harness integration

Add to `harness/`:

- `harness/encrypted-vm.sh` — variant of `launch-dfly.sh` that runs
  the create+map sequence before mkfs.
- Keyfile + passphrase baked into harness image (test-only, never
  shipped). Ops docs make clear this is non-production.
- New `tests/encrypted/` directory with E1–E11 scripts.

---

## 10. Documentation deliverables

- `docs/encryption.md` (this doc) — plan.
- `docs/workflow.md` — add encrypted-mount section once E2 passes.
- `docs/physical_disk_tasks.md` — add §"Encrypted provisioning" with
  the §4.1 recipe targeting the real hardware target.
- Operator runbook (separate, not in repo) for keyfile storage,
  rotation cadence, header backup policy.

---

## 11. Phasing

1. **Phase A** — VM-only: create+map+mkfs+mount round-trip. Land E1,
   E3, E4.
2. **Phase B** — rc.d unlock script + reboot loop. Land E2, E6.
3. **Phase C** — degraded + resilver under encryption. Land E5, E7,
   E11.
4. **Phase D** — scrub perf + LZ4 stack. Land E8, E10.
5. **Phase E** — rotate UX, runbook, hardware bring-up on Phase 3
   box. Land E9 + ops docs.

No HAMMER2 source changes anticipated at any phase. If any phase
requires modifying raid6 or chain code, the per-disk-below-raid6 model
has failed an invariant — re-evaluate before patching.

---

## 12. Open questions

- **Keyfile sourcing**: USB token, TPM-sealed blob, network KMS? Out
  of scope for v1 — pick at deploy time. Document the interface
  (`/var/h2crypt/keyfile`) and let ops slot in their preferred
  provider.
- **Boot-time prompting on headless servers**: needs serial console or
  network unlock daemon (DragonFly has no equivalent of
  `dracut-network-unlock`). Defer to ops; recommend an out-of-band IPMI
  flow.
- **Per-disk vs. shared master key**: current plan = shared unlock
  material, per-disk salts. Alternative = per-disk unique passphrase
  (N prompts) — rejected for UX. Revisit only if threat model demands
  per-disk revocation.
- **Migration path to ZFS-style native HAMMER2 encryption** (if ever
  built): tcplay layer can be peeled by full data drain + replace.
  Acceptable; no on-disk metadata commits us to tcplay.
