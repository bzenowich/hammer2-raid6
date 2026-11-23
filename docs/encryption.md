# HAMMER2 v3 RAID6: encryption at rest

**Status:** Design, revised 2026-11-22. The Pi benchmarks in §6 are measured; nothing else
here is implemented yet.
**Goal:** user data served by FlyNAS (Seafile first) is unreadable on a disk that leaves the
box: one that is stolen, RMA'd or decommissioned, or a whole stolen NAS that is powered off.
**Replaces:** the May 2026 draft. That draft assumed an x86 box with AES-NI, used command
syntax that doesn't exist, and allowed 3-disk arrays. §11 lists what changed.
**Cross-refs:** `oct-review.md` (the plan of record), `DEVELOPER.md`, `compression.md`,
`zfs_compare.md` (native encryption gap), `../../dfly/docs/dfly-arm.md` §4a (Pi throughput budget),
`../../dfly/docs/FlyNAS-plan.md` (apps, backup, SSO).

---

## 1. Summary

| Layer | What protects it | Status |
|---|---|---|
| Everything on the RAID6 array: Seafile blocks and database, VM disk images, snapshots | **dm-crypt per member disk** (`tcplay`/`dm_target_crypt`), below HAMMER2 | **Adiantum chosen 2026-11-22** (§7), with `sector_size:4096`: 51-53 / 114-156 MB/s write / read on RAID6 at 1000 MHz, 80-85% of plain (§6.7); needs a FlyNAS-owned header and unlock helper (not built yet). History: multi-core `dm_target_crypt` (§6.4) raised XTS-AES-256 from 15 / 32 MB/s (write / read) to 19 / 41 at 1200 MHz and 24 / 62 at 1000 MHz (plain: 51-64 / 150-170); the Pi's power supply, not the code, now limits it (§6.5). NEON AES-XTS and Adiantum (§6.6): cipher capacity 119-125 MB/s (XTS-AES-256) and 268-289 MB/s (Adiantum) at 1000 MHz, but on RAID6 XTS-AES-256 stays at 26 / 50-60 MB/s and Adiantum reaches 34-40 / 84-97 — the encrypted RAID6 path, not the cipher, is now the limit |
| Individual Seafile libraries, end to end | Seafile **encrypted libraries** (client-side, library password) | Optional per-user extra, not a substitute (§3.1) |
| Off-site copies | cryfs-batch (already encrypts before upload to S3) | Done |
| FlyNAS secrets (S3 keys, cryfs password) | Today in plaintext SQLite on the SD-card root | Move onto the encrypted array or wrap them (§8.4) |

The rest of this doc: the options (§3), how Google does it and what we take from that (§4–5),
measurements (§6), the recommended design (§7–9), and tests and phasing (§10).

---

## 2. What has to be protected

Seafile running on FlyNAS (as a VM app, or natively once that is decided; `FlyNAS-plan.md`
open question 1) keeps:

- its block store (file contents, content-addressed, deduplicated);
- its commit and filesystem objects (directory trees, **file names**, sizes, history);
- its database (SQLite, MySQL or MariaDB: users, shares, library metadata);
- thumbnails, preview caches, search index, logs.

Beside Seafile, the array holds VM disk images for the other apps and HAMMER2 snapshots of
all of the above. A snapshot taken before encryption was turned on keeps plaintext until it is
deleted, so encryption has to be there from `newfs` time (§9.1).

On the SD-card root: FlyNAS's SQLite database, holding user accounts, TOTP secrets, and,
in plaintext, the S3 secret keys and the cryfs password (`FlyNAS-plan.md` §2.6). The Pi has
no swap configured, so there is no swap partition leaking memory pages. If swap is ever
added it has to be encrypted too (DragonFly: `dm_target_crypt` with a random key at boot).

**Threat model.** In scope: a disk or the whole powered-off box is taken; a failed disk is
returned under RMA; disks are sold or thrown away. Out of scope: an attacker with a shell on
the running box (plaintext is in RAM and on `/dev/mapper/*`); evil-maid tampering with the
unencrypted SD card (no secure boot on the Pi); cold-boot RAM attacks; I/O-pattern leakage
(XTS shows which sectors change, and the parity layout shows the stripe structure).

---

## 3. Options

### 3.1 Seafile encrypted libraries (application level)

How it works, from Seafile's documentation (summarised from knowledge, not re-checked against
the current release):
- Opt-in per library. The library password derives a key that encrypts a random per-library
  file key. The client encrypts blocks before upload, so the server stores ciphertext blocks.
- **Not encrypted:** file and folder names, the directory structure, file sizes, modification
  times, the database, and every library that is not encrypted.
- Opening an encrypted library through the **web UI or WebDAV** sends the password to the
  server, which keeps the decrypted key in memory for a while (an hour by default). Server-side
  features (search, previews, share links, online editing) don't work on encrypted libraries.
- The library password is separate from the FlyNAS/OIDC login. A forgotten password means
  the library is lost.

**Verdict:** a good feature to offer users who want the server operator kept out, but it
doesn't meet the goal: most data and all metadata stay plaintext on disk.

### 3.2 Per-disk dm-crypt below HAMMER2 (OS level) — recommended

```
HAMMER2 v3 (RAID6, compression, checksums)
   | plaintext 64 KB columns: data, P, Q, metadata copies
/dev/mapper/h2c0  h2c1  h2c2  h2c3     <- dm_target_crypt, one mapping per member
   | ciphertext (AES-XTS, per-sector IV)
/dev/da0s1  da1s1  da2s1  da3s1        <- USB SSDs
```

- **No HAMMER2 change.** RAID6, checksums, self-heal, scrub, resilver and `h2stripe_check`
  all see plaintext devices.
- **It is the only OS-level placement.** HAMMER2's RAID6 lives inside the filesystem, so there
  is no assembled array device to put one encryption layer on top of. (The May draft argued
  against "above RAID" as if it were an option.)
- **Cost:** every byte written to any member is encrypted, so on 4 disks a write encrypts the
  data, plus P and Q (2x the data), plus 3 copies of each metadata block. Reads decrypt only the
  data columns while the array is healthy. On the Pi, with no AES instructions, this is the main
  cost; see §6.
- **Disk failure is ordinary:** a member that fails to unlock is a missing member, and RAID6
  tolerates two.
- DragonFly has the pieces in base: `tcplay(8)` and `libtcplay` (VeraCrypt-compatible headers),
  `dm(4)`, `dm_target_crypt`, `dmsetup(8)`. Ciphers: AES, Twofish and Serpent, in CBC (with
  `essiv`/`plain`/`plain64` IVs) or XTS. Upstream has no ChaCha20 or Adiantum; branch
  `dmcrypt-mp` adds Adiantum and a NEON AES-XTS for arm64 (§6.6).

### 3.3 Encrypt inside the Seafile VM (guest LUKS2)

If Seafile runs in a Linux VM, the guest can put its data disk under LUKS2. Each byte is
encrypted once (no parity), and Linux has Adiantum (XChaCha12 + Poly1305-based hashing), built
for ARM chips without AES. The catch: it covers only that VM, it needs an unlock path into
the guest at boot, and on the Pi there are no VMs yet (NVMM is x86-only; `dfly-arm.md` §4c).
Keep it in mind for when the arm64 VMM exists; not the baseline.

### 3.4 Native HAMMER2 encryption (future)

Encrypt data and metadata blocks inside HAMMER2 before the RAID6 layer: parity is then computed
over ciphertext and never encrypted separately, so the write cost drops to the data plus one
metadata copy's worth. Copy-on-write helps: every block write is a new physical block, so a
nonce from the block reference (TXG plus logical offset) never repeats, and an AEAD cipher's tag
can replace or sit next to the XXH64 check. Per-PFS keys would allow per-user or per-library
crypto-shredding (§5). This is months of on-disk-format work (bref fields, key wrapping in the
volume header, scrub and resilver on ciphertext, `fsck`, a new layout number) and the kind of
change §11 of the May draft said not to make. Recorded as the long-term option, not planned.

### 3.5 Self-encrypting drives (TCG Opal)

The 850 EVOs support Opal; the BX500s, as far as I know, don't. Opal commands would have to pass
through the JMicron USB bridges, and DragonFly has no `sedutil`. Not usable with this hardware.

---

## 4. How Google encrypts data at rest (Colossus and its key management)

Colossus is Google's cluster-level distributed file system (the successor to GFS). Bigtable,
Spanner and Cloud Storage are layered on top of it. Google's public encryption documentation
doesn't name Colossus; it speaks of "storage systems" and the "distributed file system", which is
the layer Colossus provides. The facts below are from Google Cloud's *Default encryption at rest*
page (updated May 2024) and *Cloud KMS deep dive* (updated September 2026); see Sources.

### 4.1 Data path

- **Chunks.** A storage system splits data into chunks ("each chunk can be up to several
  gigabytes in size") and encrypts each chunk with its **own data encryption key (DEK)**:
  "two chunks won't have the same DEK". An **updated chunk is re-encrypted with a new DEK**
  instead of reusing the old one, which limits what one leaked DEK exposes. Shared objects
  (such as a public base image) are stored once, under one DEK.
- **Cipher.** AES-256 by default (some Persistent Disks from before 2015 still use AES-128),
  AES-GCM preferred. The crypto library is BoringCrypto (FIPS 140-2 validated), used through Tink.
- **Wrapped DEKs are stored next to their chunk.** To read a chunk, the storage system sends
  the wrapped DEK to the key service to unwrap. Each chunk is replicated in encrypted form.
- **Three layers.** Storage-system (chunk) encryption above; storage encryption in the
  distributed file system and database layers; and **device-level encryption** on every HDD and
  SSD in production, AES-256, with a separate device key. A bug or leak in one layer leaves
  the other two.

### 4.2 Key hierarchy

| Key | Wraps | Lives in |
|---|---|---|
| DEK (per chunk) | the chunk's data | stored *wrapped* with the chunk |
| KEK (one or more per service, not per customer) | DEKs | **Keystore** (formerly "Google's internal KMS"), replicated thousands of times on ordinary production machines; KEKs never leave it |
| Keystore master key (AES-256) | all KEKs in Keystore | **Root Keystore**: a few dedicated machines per data center |
| Root Keystore master key (AES-256) | Root Keystore's keys | **root keystore master key distributor**: a peer-to-peer service holding it **in RAM only** on dedicated machines, synchronised by gossip, no central node |
| (backup of the root key) | | secure hardware devices in **physical safes** in a few locations, open to a few employees; used only if every distributor instance in a region goes down at once |

### 4.3 Key management practices

- **Envelope encryption.** DEKs are random and only ever stored wrapped. Rotating a KEK means
  rewrapping DEKs, never re-encrypting data. Keystore rotates KEKs automatically and keeps old
  versions for decryption. Cloud KMS documents a 90-day maximum ciphertext age under the
  Keystore master key, re-encrypts customer keys monthly, and refreshes cached master keys daily.
- **DEKs are not rotated on a schedule.** A chunk gets a new DEK when it is rewritten.
- **Access control at a chokepoint.** Keystore unwraps only for an authenticated service whose
  per-key ACL allows that KEK, and checks the job is allowed that chunk (by job and chunk id).
  Every key operation is logged with the caller's identity.
- **Backups are encrypted separately.** Most backup files get their own DEK, derived from a key in
  Keystore and a random per-file seed.
- **Separation by customer is optional.** Default KEKs are shared across customers. A customer who
  wants their own keys uses Cloud KMS (CMEK), Cloud HSM or an external key manager, and can then
  destroy a key (30-day scheduled destruction by default) to make the data unreadable.

---

## 5. What FlyNAS takes from Google's design

| Google | FlyNAS equivalent |
|---|---|
| Random DEKs, wrapped by KEKs (envelope) | The tcplay/VeraCrypt header already does this: a random master key per disk, encrypted by a key derived from the passphrase (PBKDF2). Changing the passphrase rewrites the header, never the data (§9.5) |
| Wrapped DEK stored with the chunk | The wrapped master key is the header at the start of each disk, with a backup header in its last 128 KB |
| Root key in RAM only; backup in physical safes | The passphrase (or the key it unlocks) is in RAM only after unlock. The **recovery kit** (printed recovery passphrase plus header backups, §9.2) is the "safe" |
| Three independent layers | dm-crypt (device level), Seafile encrypted libraries (application level, optional), cryfs-batch (backups) |
| Backups under their own keys | cryfs-batch has its own password, separate from the disk passphrase; losing one doesn't expose the other |
| Chokepoint with ACL and audit log | One unlock path: the setuid FlyNAS helper. Every unlock attempt (success, failure, source address) goes into the FlyNAS audit log |
| New DEK whenever a chunk is rewritten | Not possible with dm-crypt (fixed key per disk, IV from the sector number). Would come with native HAMMER2 encryption (§3.4): CoW gives a fresh nonce per block write |
| Per-customer keys and key destruction | Per-disk keys give no per-user separation. Per-PFS keys (native, §3.4) would allow crypto-shredding one user or library, snapshots included |

The two Google practices dm-crypt cannot match are **per-chunk keys** and **per-tenant keys**.
Neither matters against the threat model in §2 (whole disks or a whole box taken). Both would
matter for "delete this user's data provably, including from snapshots and resilvered disks",
which is the argument for §3.4 one day.

---

## 6. Measurements on the Pi 4 (2026-11-22)

Kernel #56 (k56, `85d170c76b` + RAID6 overlay), 1200 MHz, `hw.physmem=2g`, 4 Cortex-A72 cores
with **no ARMv8 Crypto Extensions**, so AES is the generic C `rijndael` code. Modules `dm`,
`dm_target_crypt` and `crypto` were cross-built for this kernel
(`MODULES_OVERRIDE="crypto dev/disk/dm" DFLY_SRC=tools/r6src bin/arm-kbuild -n ARM64_RPI4_R6 modules-depend modules`)
and installed in `/boot/modules.local`; they load without problems.

### 6.1 Cipher cost alone (no disks)

A `dm` `zero` target (reads return zeros, writes are discarded) under a `crypt` mapping, `dd`
1 GB at `bs=1m`. That measures only the cipher and dm overhead.

| Mapping | Write MB/s | Read MB/s |
|---|---|---|
| zero target, no crypt (floor) | 950 | 2015 |
| `aes-xts-plain64`, 512-bit key (XTS-AES-256) | 37 | 37 |
| `aes-xts-plain64`, 256-bit key (XTS-AES-128) | 46 | 46 |
| `aes-cbc-essiv:sha256`, 256-bit key | 43 | 42 |
| `aes-cbc-plain64`, 128-bit key | 57 | 57 |
| `twofish-xts-plain64`, 512-bit key | 38 | 38 |
| `serpent-xts-plain64`, 512-bit key | 27 | 27 |
| 4 XTS-AES-256 mappings, 4 `dd` at once | 34+36+34+33 = **137** | 36+37+37+37 = **147** |

These are the original single-threaded module. The multi-core module (§6.4) on the same
mappings, one `dd` stream, `kern.dm_crypt.mp=1`:

| Mapping | Write MB/s | Read MB/s | Note |
|---|---|---|---|
| XTS-AES-256 | 66-69 | 70-76 | sustained, under-voltage throttled (§6.5) |
| XTS-AES-256, first seconds before throttling | 131 | 105 | 3.5x one core |
| XTS-AES-256 at 1000 MHz (no throttling) | 110 | 112 | steady |
| XTS-AES-128 | 107 | 166 | throttling intermittent during this run |
| CBC-AES-256-ESSIV | 121 | 155 | " |
| CBC-AES-128 | 193 | 204 | " |
| Twofish-XTS-256 | 136 | 140 | " |
| Serpent-XTS-256 | 98 | 104 | " |

With `kern.dm_crypt.mp=0` (old path) the same build gives the single-core numbers above, and
four parallel streams drop to 19 MB/s each (76 total, vs 137 in the first run): all four cores
busy browns out the supply and the firmware halves the clock (§6.5).

- One stream is limited to one core: `dm_target_crypt` encrypts synchronously in the thread that
  issues the write (and decrypts in the thread that completes the read) through `cryptoapi`.
  Four streams scale almost linearly to four cores.
- Userland LibreSSL 3.6.1 (built without assembly on arm64), `openssl speed -evp`, 16 KB blocks,
  one core: AES-128-GCM 39 MB/s, AES-256-GCM 33 MB/s, AES-256-CBC 54 MB/s, AES-256-XTS 52 MB/s.
  The kernel's XTS (37 MB/s) is about 30% slower than userland's. ChaCha20-Poly1305 is not in
  this LibreSSL's `speed`.

### 6.2 HAMMER2 RAID6 on the four SSDs, with and without encryption

`tests/perf/crypt_bench.sh` (the cipher-only run in §6.1 is `tests/perf/crypt_cipher_bench.sh`): four dm mappings over the first 4 GB of `da0s1..da3s1`, either
`linear` (the baseline, no cipher) or `crypt`; `newfs_hammer2 -R 6`; write two 512 MB files of
random data (from tmpfs) and `sync`, timed to the second; remount; cold-read each file with `dd`.
Default compression (LZ4 gives up on random data), async seal on. Two passes, in order
plain, XTS-AES-256, XTS-AES-128, CBC-AES-128.

| Mapping | Write MB/s (pass 1, 2) | Cold read MB/s (4 reads) | vs plain (write / read) |
|---|---|---|---|
| plain (`linear`) | 51, 64 | 171 172 171 169 | 1 / 1 |
| XTS-AES-256 (what tcplay uses) | 14, 15 | 31 33 32 31 | 0.25 / 0.19 |
| XTS-AES-128 | 17, 17 | 40 39 40 39 | 0.30 / 0.23 |
| CBC-AES-128 (plain64 IV) | 20, 20 | 49 46 41 41 | 0.35 / 0.26 |

`vmstat` showed the CPU about 64% idle in **every** run, encrypted or not. So the encrypted
array is not limited by the four cores together. `top -S` during an XTS-AES-256 run shows why:

- **Reads:** one interrupt thread (`ithread195`, the USB controller's completion thread, on CPU 3)
  at **93%**. `dm_target_crypt` decrypts in `biodone`, which runs in that thread for all four disks.
  31-33 MB/s is one core's worth of XTS-AES-256 (37 MB/s in §6.1), minus the rest of that thread's work.
- **Writes:** four HAMMER2 XOP worker threads (`h2xop-BENCH.03/07/11/15`), all bound to CPU 3, at
  15-34% each, adding up to about one core. `dm_target_crypt` encrypts in the thread that calls
  `strategy`, and for one file HAMMER2 issues those writes from one CPU's XOP group. 15 MB/s of user
  data is about 33-37 MB/s encrypted once P, Q and the metadata copies are counted (§3.2), again
  one core.

After the multi-core change (§6.4), `kern.dm_crypt.mp=1`, same script:

| Mapping | Clock | Write MB/s (passes) | Cold read MB/s | Avg idle | vs old (write / read) |
|---|---|---|---|---|---|
| plain | 1200 | 64, 60 | 171 171 152 170 | 65% | — |
| XTS-AES-256 | 1200 | 19, 19 | 41 40 41 40 | 25% | 1.3x / 1.3x |
| XTS-AES-128 | 1200 | 22, 21 | 46 45 47 45 | 30% | 1.3x / 1.15x |
| CBC-AES-128 | 1200 | 23, 23 | 49 44 51 45 | 34% | 1.15x / 1.05x |
| plain | 1000 | 51 | 140 153 | 63% | — |
| XTS-AES-256 | 1000 | 24 | 65 59 | 32% | **1.6x / 1.9x** |
| XTS-AES-128 | 1000 | 29 | 57 58 | 36% | 1.7x / 1.5x |

At 1200 MHz the supply browned out repeatedly (16 under-voltage events in the 1200 MHz run, 9 at
1000 MHz), so the 1200 MHz rows are the throttled clock most of the time. The gain is now
limited by power, not by the code: the cipher-only test reaches 131 MB/s for XTS-AES-256
before the throttle hits.

### 6.3 What the numbers mean

- **The cipher is not the whole problem; using one core for it is.** dm-crypt on the Pi runs at
  one core's speed (37 MB/s for XTS-AES-256) because it encrypts and decrypts in whatever thread
  happens to issue or complete the I/O, and on this system that is one thread. Four cores give
  137-147 MB/s (§6.1).
- **As it stands, encryption costs 75-80%:** writes 51-64 to 14-15 MB/s, reads 170 to about 32 MB/s.
  Reads drop below what one gigabit client could pull, and writes to about a quarter of today's.
- **XTS-AES-128 or CBC buy 15-35%,** not enough to change the picture, and tcplay can't use them.
- **Where the ceiling is after a fix:** if dm-crypt spread its work over all four cores, reads could
  reach roughly 120-140 MB/s (the 147 MB/s cipher limit minus the USB and HAMMER2 work that shares
  the cores), close to gigabit speed. Writes encrypt about 2.2x the user data on 4 disks, so they
  would top out near 60 MB/s of cipher capacity and land around 35-50 MB/s after HAMMER2, LZ4 and
  parity take their share. These were estimates from §6.1 and the plain runs; the measured results are in §6.4-6.5.
- **Faster per core:** a NEON bitsliced AES-XTS (Linux's `aes-neon-bs` approach, which processes
  8 blocks at once with NEON) is reported to roughly double generic AES on Cortex-A72-class cores
  without crypto instructions. Adiantum (XChaCha12 + NH/Poly1305, designed by Google for Android
  devices without AES instructions) is reported at several times generic AES on the same class
  of cores. Both are now implemented and measured (§6.6): on the A72, NEON AES-XTS is 1.1-1.3x
  the C code per core (more on encrypt than decrypt), Adiantum NEON 2.3-3x C XTS-AES-256.

### 6.4 Multi-core `dm_target_crypt` (2026-11-22)

Branch `dmcrypt-mp` in `dfly/tools/r6src` (uncommitted; a copy is in `hammer2-raid6/dmcrypt_mp.patch`), two files:

- `sys/dev/disk/dm/crypt/dm_target_crypt.c`: one worker thread per CPU (`dmtc N`). A bio's buffer
  is split into sector-aligned pieces (at least `kern.dm_crypt.piece_min` = 16 KB, at most one per
  worker) queued round-robin; the worker that finishes the last piece issues the encrypted write
  or completes the read. Per-request state lives after the data in the existing `mpipe` buffer,
  so nothing new is allocated per I/O and memory stays bounded. `kern.dm_crypt.mp=0` restores the
  old path for A/B; crash dumps always use the synchronous path. No on-disk change.
- `sys/crypto/cryptoapi/cryptoapi.c`: **bug fix.** The generic AES-XTS, Twofish-XTS and
  Serpent-XTS code kept the per-sector tweak in the shared session context, so two threads using
  one session overwrote each other's tweak. Multi-core decryption returned garbage until the tweak
  moved to the stack. The original single-threaded module has the same race whenever two threads
  write to one mapping, or two reads complete on different interrupt threads, at the same time,
  on any machine without AES-NI (the AES-NI path takes the IV per call and is safe). CBC is
  stateless and was never affected. This should go upstream regardless of the multi-core change.

Verified on the Pi for XTS-AES-256, XTS-AES-128, Twofish-XTS and Serpent-XTS (and CBC-ESSIV):
data written by the old path reads back through the new one and vice versa; ciphertext written
by both paths is byte-identical (no format change); uneven splits (65-sector bios, 512-byte
pieces); four mappings written and read concurrently. All 12 checks pass per cipher.

### 6.5 The Pi's power supply limits four-core work

`dev.bcmmbox.0.throttled` goes from `0x50000` to `0x50005` (under-voltage now, ARM throttled now)
within seconds of all four cores running AES. The SoC was 60-64 °C, so this is power, not heat.
`dev.bcmmbox.0.arm_mhz` keeps reporting the requested 1200 MHz because it reads
`GET_CLOCK_RATE`, not the measured clock (`GET_CLOCK_RATE_MEASURED`, tag 0x00030047, would show
the throttle; worth adding). One XTS-AES-256 stream through four workers:

| `arm_mhz` | Read MB/s | Write MB/s | Under-voltage |
|---|---|---|---|
| 1200 (fresh) | 105 | 131 | yes, after a few seconds |
| 1000 | 112 | 110 | none |
| 900 | 101 | 100 | none |
| 800 | 89 | 89 | none |
| 1200 (sustained) | 69 | 69 | yes, most of the time |

On the current 2.4 A supply, 1000 MHz gives more sustained crypto throughput than 1200. The
official 5.1 V / 3 A supply (or a powered USB hub for the four SSDs, which share the Pi's 5 V
rail) should allow 1200 without throttling; then expect about 130 MB/s of cipher capacity.

### 6.6 NEON AES-XTS and Adiantum (2026-11-22)

Same branch (`dmcrypt-mp`, uncommitted; included in `hammer2-raid6/dmcrypt_mp.patch`). New files:

- `sys/crypto/aes_neon/aes_neon.[ch]`: AES-128/256 for ARMv8 Advanced SIMD **without** the Crypto
  Extensions. XTS runs 8 blocks at a time **bitsliced** (the `aes-neon-bs` approach): a SWAPMOVE
  transpose puts bit *i* of every state byte into register *i*, the S-box is the 113-gate
  Boyar–Peralta circuit (inverse S-box through the affine maps), ShiftRows and MixColumns are byte
  shuffles (`TBL`, `REV32`) plus XORs, and the round keys are pre-expanded into bitsliced masks at
  setkey. Single blocks (the XTS tweak) use a `TBL`-based table-free path. No data-dependent table
  lookups, so it is also constant-time, which the C `rijndael` is not. A partial last batch is
  zero-padded. Ciphertext is standard XTS (identical to the C code, and to LibreSSL
  `openssl enc -aes-256-xts`), so tcplay volumes are unaffected.
- `sys/crypto/adiantum/adiantum.[ch]`: portable Adiantum (XChaCha12, NH + Poly1305 hash, one
  AES-256 block per sector), per the Google reference; and `adiantum_neon.c`: 4-way ChaCha12 and
  NH in NEON. dm table name `xchacha12,aes-adiantum-plain64` with a 256-bit key, the same as
  Linux, whose tweak (the 16-byte IV) it matches.
- `cryptoapi.c`: two new ciphers, registered ahead of the generic ones: `aes-xts` resolves to the
  NEON version on arm64. Both self-test at first use (NEON XTS against the C XTS for both key
  sizes, both directions; Adiantum against a Google known-answer vector and NEON against C) and
  refuse to register on a mismatch. Calls are bracketed by `kernel_fpu_begin()`/`kernel_fpu_end()`
  per sector. `hw.neon_crypto_disable=1` (tunable and sysctl, applies to new mappings) falls back
  to C for A/B tests.
- Build: the kernel is `-mgeneral-regs-only`; the two NEON files are compiled without it (rules in
  `sys/crypto/Makefile` and `sys/platform/arm64/conf/files`, the latter not yet tried in a full
  kernel build).

Tests: 180 / 180 Adiantum reference vectors (C and NEON); NEON AES against `rijndael` on 2000
random blocks per key size and FIPS-197; XTS against the reference for many lengths; Adiantum NEON
= C on 500 random messages. In the kernel, the 12 `dm` checks of §6.4 for XTS-AES-256, XTS-AES-128
and Adiantum, plus NEON-written data read back through C and vice versa.

**One core, userland, 1200 MHz (MB/s, 512-byte / 4096-byte sectors):**

| Cipher | 512 B | 4096 B |
|---|---|---|
| XTS-AES-256, C (`rijndael`) | 48 | 49 |
| XTS-AES-256, NEON (encrypt / decrypt) | 56 / 47 | 63 / 52 |
| XTS-AES-128, NEON (encrypt / decrypt) | 76 / 64 | 85 / 70 |
| Adiantum, C | 100 | 130 |
| Adiantum, NEON | 128 | 199 |

On the A72 the bitsliced AES is throughput-bound on the NEON pipes (about 185 vector ops per round
per 8 blocks at 1.4 Q-ops/cycle); inverse MixColumns costs more than forward, so decrypt gains
less. Without the Crypto Extensions, AES has little more to give on this core; Adiantum's ChaCha
is a better fit.

**Cipher alone (dm `zero` target, `tests/perf/crypt_cipher_bench.sh`), MB/s write / read:**

| Mapping | 1200 MHz, mp=0 (one core) | 1200 MHz, mp=1 (throttled) | 1000 MHz, mp=1 |
|---|---|---|---|
| zero, no crypt | 666 / 1611 | 1044 / 1945 | 752 / 1645 |
| XTS-AES-256 NEON | 48 / 41 | 88 / 77 | 119 / 125 |
| XTS-AES-256 C | 37 / 37 | 72 / 69 | 111 / 113 |
| XTS-AES-128 NEON | 64 / 55 | 112 / 99 | 178 / 163 |
| XTS-AES-128 C | 46 / 46 | 85 / 85 | 135 / 137 |
| **Adiantum NEON** | **109 / 111** | **278 / 341** | **268 / 289** |
| Adiantum C | 76 / 78 | 241 / 258 | 203 / 215 |
| CBC-AES-256-ESSIV | 43 / 43 | 79 / 79 | 126 / 129 |
| 4 streams XTS-AES-256 NEON (sum) | 96 / 84 | 94 / 83 | 148 / 126 |
| 4 streams Adiantum NEON (sum) | 211 / 227 | 182 / 208 | 241 / 343 |

The 1200 MHz mp=1 column browned out (`throttled` 0x50005) and is lower than 1000 MHz for AES.

**HAMMER2 RAID6 (`tests/perf/crypt_bench.sh`), 1000 MHz, mp=1, two passes, MB/s:**

| Mapping | Write | Cold read | Avg idle (write) |
|---|---|---|---|
| plain | 56, 51 | 152 116 144 136 | 61-63% |
| XTS-AES-256 NEON | 22, 25, 26, 26 | 42 40 51 49 50 43 48 54 | 29-34% |
| XTS-AES-256 C | 23, 26, 26, 27 | 56 59 60 61 56 62 60 61 | 29-31% |
| XTS-AES-128 NEON | 30, 31 | 49 49 70 53 | 37-39% |
| **Adiantum NEON** | **40, 34** | **86 84 97 90** | 43-54% |

(The extra XTS-AES-256 rows are a second run with C first, to rule out order effects.)

What this shows:

- **On RAID6 the cipher is no longer the limit.** The cipher-only test gives XTS-AES-256 119 MB/s
  and Adiantum 268 MB/s, yet RAID6 writes are 26 and 34-40 MB/s with the CPU a third to half idle.
  Writes encrypt about 2.2x the user data (§3.2), so even XTS-AES-256 uses only half its capacity.
  What remains is per-request latency through `dm_target_crypt` (one 64 KB HAMMER2 block split
  across workers, then the encrypted write issued) and HAMMER2's limited write concurrency per
  file, as §7 item 2 already suspected. Next step there, not the cipher.
- **NEON XTS gains nothing on RAID6, and reads are about 15% slower than C** (43-54 vs 56-62 MB/s,
  repeated with the order swapped; read CPU idle 29% vs 33%). The bitsliced decrypt is barely
  faster than C per sector at 512 bytes, and something in the RAID6 read path (cache pressure from
  the USB and HAMMER2 work, or the per-sector FPU save/restore) tips it under. Not root-caused. Until
  it is, XTS on the Pi is just as well served by the C code; the NEON path's remaining merit is
  constant-time operation. A cheap follow-up is NEON for encrypt and C for decrypt (the ciphertext
  is the same).
- **Adiantum is the clear win:** 1.5x XTS-AES-256 writes and reads on RAID6, 2.2-3x in the cipher
  test, and it does not need four cores to get there (109 MB/s on one). Its security rests on
  XChaCha12 and AES-256 (one block per sector), and it is a wide-block mode: one changed bit
  changes the whole sector's ciphertext, which is better than XTS's 16-byte granularity.
- **But Adiantum cannot use tcplay.** VeraCrypt headers know only AES/Twofish/Serpent/Camellia/
  Kuznyechik in XTS. An Adiantum array needs its own unlock path: a small header (salt, KDF
  parameters, the wrapped volume key) read by FlyNAS's unlock helper, which then calls `dmsetup`
  with the derived key; the key model in §8.1 stays the same. That is a modest amount of code but a
  format we own.
- **Larger sectors would help both.** Adiantum NEON is 199 MB/s per core at 4096-byte sectors vs
  128 at 512, and XTS gains about 12%. Linux's dm-crypt has `sector_size:4096`; adding that option
  to `dm_target_crypt` (HAMMER2 never writes less than 4 KB, but the members must be 4 KB-aligned
  and the option is fixed at creation) is worth doing together with an Adiantum header.

### 6.7 `sector_size:4096` and the hot-CPU fix (2026-11-22)

**`sector_size`.** `dm_target_crypt` now takes Linux's optional arguments after the device offset:
`<#opt> sector_size:N [iv_large_sectors]`, N a power of two from 512 to PAGE_SIZE. Each N-byte
sector is one cipher call; the IV is the 512-byte sector number (relative to the target, plus
`iv_offset`), or that number divided by N/512 with `iv_large_sectors`, as on Linux, so volumes can
move between the two. I/O not aligned to N fails with EINVAL (rate-limited message); the dm device
still reports 512-byte blocks, so `disklabel`/probe reads of 512 bytes are refused, harmlessly.
HAMMER2 newfs, mount and the bench run unchanged on 4 KB mappings. Table example:

    0 8388608 crypt xchacha12,aes-adiantum-plain64 <64 hex> 0 /dev/da0s1 0 1 sector_size:4096

Test (`ss.sh`, Pi, 92 / 92): ciphertext on disk equals a userland reference encryptor (Adiantum
and XTS, 512 / 4096, with and without `iv_large_sectors`, non-zero `iv_offset`), for NEON on and
off and single/multi-core; reads back at 1 MB and 4 KB; 512-byte I/O on a 4 KB mapping refused;
bad `sector_size` and misaligned `iv_offset` with `iv_large_sectors` refused at create.

**Where the time went.** With top/iostat/vmstat samples during the RAID6 run (1000 MHz), the
machine was ~55% idle but **one CPU was at 100%**: on writes CPU 3 ran `dd`, all four h2xop
threads that handled its blocks (~57%), its `dmtc` worker, and both USB interrupt threads; on
reads the same pattern on the reader's CPU. Two causes:
1. HAMMER2 routes strategy (file data) XOPs to the threads **of the calling CPU**
   (`hammer2_xop_start_except`: `ng = mycpu % mod + mod*which`), so one sequential writer or
   reader does all its compression, check codes and RAID6 parity on one core.
2. `dm_target_crypt` dealt pieces round-robin to per-CPU queues, so a quarter of every bio waited
   for the saturated CPU's worker.

**Fixes.**
- HAMMER2 (`src/sys/local_hammer2_admin.c`, new in the overlay; sysctl
  `vfs.hammer2.spread_strategy`, default 1): strategy XOPs pick a thread by
  `(ihash + lbase >> PBUFRADIX) % (mod * sgroups)`, so consecutive 64 KB blocks rotate over all
  CPUs. Ordering is unchanged: blocks of one file already ran concurrently on several threads.
  0 restores upstream behaviour. Kernel #57.
- `dm_target_crypt`: one shared piece queue; the dispatcher wakes idle workers on other CPUs
  first, and any worker that finishes takes the next piece. `kern.dm_crypt.piece_min` default
  16 KB -> 4 KB (16 pieces per 64 KB block).

**Results** (1000 MHz, `crypt_bench.sh`, two passes, write = 1 GB + sync, cold read of each file):

| | write MB/s | read MB/s | idle (write) | before (§6.6) |
|---|---|---|---|---|
| plain | 64, 64 | 214 172 196 171 | 60% | 51-56 / 116-152 |
| XTS-AES-256 NEON | 40, 36 | 70 70 59 62 | 18% | 22-26 / 40-54 |
| XTS-AES-256 C | 34, 35 | 70 78 68 74 | 16% | 23-27 / 56-62 |
| XTS-AES-128 | 32, 35 | 69 67 63 67 | 20% | |
| Adiantum 512 | 53, 51 | 138 114 139 128 | 28% | 34-40 / 84-97 |
| **Adiantum 4096** | **53, 53** | **142 139 156 124** | 33% | |

A/B of each fix alone (`prof.sh`, one file each way, noise about +-10%): spreading XOPs took plain
reads 146 -> 170 and Adiantum-4K reads 113 -> 128, writes flat; the shared queue took Adiantum-4K
from 44 / 102 to 53 / 113. `throttled` showed only the sticky under-voltage bits (0x50000) at
the end, nothing live.

**What limits it now.**
- XTS is CPU-bound (16-20% idle): AES without the Crypto Extensions, as §6.3 predicted.
- Adiantum writes stop at ~53 MB/s with a third of the CPU idle; `dd` sleeps in `flstik`
  (`bd_wait`, the buffer cache's dirty-space throttle), so the writer is waiting for device
  completions. Remaining overheads in the profile: `usbus0` 17-23% of a core, `pagedaemon`
  15-28% (the bench keeps a 512 MB tmpfs source in 2 GB of RAM, so part of that is the test),
  and the `random` thread ~7%. Plain RAID6 writes are themselves only 64 MB/s, so encryption
  now costs ~17% on writes and 20-30% on reads.
- NEON XTS reads are no longer slower than C within noise; the §6.6 anomaly was probably the
  hot CPU.

---

## 7. Recommendation

1. **Baseline: per-disk dm-crypt with tcplay headers (§3.2), XTS-AES-256, `SHA512-VC`.** It is the
   only option that covers all the data (Seafile, databases, VMs, snapshots) with no HAMMER2
   change, and it works on the Pi today. Offer Seafile encrypted libraries as an optional
   per-user extra (§3.1). Keep cryfs-batch for backups.
2. **Done 2026-11-22 (§6.4), not yet committed or upstreamed: `dm_target_crypt` uses all cores.**
   Measured: reads 32 to 41 MB/s and writes 15 to 19 MB/s at 1200 MHz on the current supply;
   62 / 24 MB/s at 1000 MHz. Lower than the estimate below because the power supply throttles the
   clock (§6.5); fix the supply (or run at 1000 MHz) before judging the remaining gap. The design:
   - hand each bio's encrypt (on write) or decrypt (on read completion) to per-CPU worker threads
     instead of running it in the caller or the interrupt thread;
   - split a large bio into sector-aligned pieces across the workers (XTS and `plain64`/`essiv`
     IVs are per sector, so pieces are independent) and complete the bio when the last piece is done;
   - keep the `mpipe` buffer limits so memory stays bounded.

   The pre-change estimate was reads 120-140 MB/s and writes 35-50 MB/s. Even at 1000 MHz the
   RAID6 runs stay 32% idle, so per-bio latency (one 64 KB HAMMER2 block split four ways) is part
   of what remains; deeper read-ahead or larger pieces are the next things to try. It also helps x86 boxes without AES-NI, so it is a
   reasonable upstream DragonFly patch (oct-review: add as a new item under §9).
3. **Decided 2026-11-22: Adiantum with `sector_size:4096`** (`xchacha12,aes-adiantum-plain64`,
   one 256-bit key per member). Implemented and tested, uncommitted (§6.6, §6.7): 51-53 / 114-156
   MB/s on RAID6 at 1000 MHz vs 32-40 / 59-78 for XTS. It replaces item 1's tcplay/XTS baseline for
   the array. Cost: tcplay cannot describe it, so FlyNAS needs its own on-disk header (salt, KDF
   parameters, wrapped volume key, cipher and sector size) and an unlock helper that derives the
   key and calls `dmsetup`; the key model in §8 is unchanged, and §9's tcplay commands become that
   helper's. The header format and helper are the next step. XTS (tcplay) remains a fallback.
4. **Not now:** guest LUKS (§3.3) waits for an arm64 VMM; native HAMMER2 encryption (§3.4) is
   the long-term route to per-user keys and to not encrypting parity, and would be a separate design.
5. **Hardware note:** a board with ARMv8 Crypto Extensions (e.g. a Pi 5's Cortex-A76) would make
   AES-XTS cost a few percent of one core instead of a whole one. The port targets the Pi 4, so
   (2) is needed regardless.

---

## 8. Keys and unlocking on FlyNAS

Since the switch to Adiantum (§7 item 3) the members carry FlyNAS's own header, written and read by
`flcrypt` (`dfly/overlay/usr/local/flynas/src/flcrypt/`, installed as `/usr/local/sbin/flcrypt`).
tcplay remains possible for an XTS array, but the text below describes flcrypt. The design follows
LUKS (key slots, envelope encryption) with only what a RAID6 member needs, and it builds plain
dm-crypt tables, so the same table works on Linux (`dmsetup` with the same cipher name and
`sector_size:4096`): flcrypt also builds on Linux with OpenSSL, which is the recovery path off
DragonFly.

### 8.1 Key model

- **One master key per member**, 256 random bits (Adiantum takes one 256-bit key; 512 for XTS),
  used directly as the dm-crypt key. It never leaves the box except wrapped in a slot.
- **Eight key slots per member.** Each slot wraps the master key with AES-256-GCM under a key
  derived from one secret: a **passphrase** (PBKDF2-HMAC-SHA512, iterations calibrated at
  `format`/`addkey` time to 1 s on the machine running it), a **recovery key** (160 random bits
  shown once as 32 base32 characters, `XXXX-XXXX-...`; 1000 iterations are enough at that entropy),
  or a **keyfile** (at least 32 bytes, for unattended boot). Any slot opens the member, and each
  slot can be added, changed or removed without touching data (Google's KEK/DEK split, §4.2).
  This fixes the tcplay limitation of one passphrase per header: the admin passphrase and the
  recovery key are separate slots, and losing one doesn't lock out the other.
- **One passphrase for the array.** `format` and `addkey` take the members as a list and write the
  same secrets to all of them, each with its own master key, salts and nonces. One member's header
  and the passphrase do not decrypt another member's data without that member's header too.
- **Authenticated metadata.** The GCM additional data is the header's fixed part (array and member
  UUIDs, cipher, key size, sector size, data offset and size, creation time) plus the slot's index
  and parameters. Changing any of them, or copying a slot to another member or position, makes
  every slot fail to open, so an attacker who can write the disk cannot make the box map it with a
  weaker cipher or a shifted data area. A SHA-256 over the whole header detects plain damage
  without the passphrase.
- **Why PBKDF2 and not Argon2:** PBKDF2-HMAC-SHA512 is in the base system's LibreSSL and in every
  OpenSSL; Argon2 would mean importing it. The slot has a KDF field, so Argon2id can be added as
  KDF 2 later without a format change. With 1 s of PBKDF2 per guess on the Pi, a passphrase of four
  or more random words is out of reach of a GPU farm; the UI should insist on that strength.
- **Why GCM is fine here** (unlike for the data, §8.1 of the XTS discussion above): each slot is
  encrypted once per change with a fresh random nonce under a key used for nothing else.

### 8.2 The header (version 1)

4096 bytes, little-endian, at byte 0 of the slice and again in its last 4 KB. The data area (the
dm mapping) runs from 1 MB to 1 MB before the end, so the copies and room for a future larger
header are outside it; `format` zeroes both 1 MB areas, which also destroys an old HAMMER2 or
tcplay header there. IV sector numbers count from the start of the data area. Of two valid copies
the higher `seq` wins; every change bumps `seq` and writes the backup copy first, then the primary.

| Offset | Field | |
|---|---|---|
| 0 | magic `FLYNASCR`, version 1, header size 4096 | |
| 16 | seq | not in the AAD |
| 24 | array UUID, member UUID, member index, member count, label (32) | fixed part (AAD) |
| 96 | cipher (64, the dm name, e.g. `xchacha12,aes-adiantum-plain64`) | fixed |
| 160 | key bytes, sector size, flags (`iv_large_sectors`), data offset, data size, created | fixed |
| 256 | 8 slots x 160: type, KDF, iterations, salt (32), nonce (12), wrapped key (64), tag (16), name (16) | |
| 1536 | key check: HMAC-SHA256(master key, "flcrypt-check" + member UUID) | |
| 4064 | SHA-256 of bytes 0-4063 | |

The mapping `open` creates for member *i* is `/dev/mapper/h2c<i>` (the index comes from the header,
so the order of device names, which USB can shuffle, doesn't matter):

    0 <data_size/512> crypt <cipher> <hex key> 0 <slice> 2048 1 sector_size:4096

The key goes to the kernel through libdevmapper, never through a command line, and the kernel's
table status shows it zeroed. Secrets live in `mlock`ed memory and are wiped after use.

### 8.3 Boot sequence

1. The SD-card root boots as today: network, sshd, OpenResty, FlyNAS UI. No user data is on it.
2. `/usr/local/etc/rc.d/flcrypt` (`flcrypt_devices`, `flcrypt_mount="DATA /data"`) opens the members
   with `flcrypt_keyfile` if one is set and readable (unattended boot; protects only against
   theft of the disks without the key medium, and the UI must say so), or asks on the console if
   someone is there (three tries), or leaves the array locked.
3. Locked: FlyNAS shows an **Unlock storage** page to an authenticated admin (TOTP login) and pauses
   everything that needs the array (app VMs, Seafile, snapshot and scrub cron jobs, backups). The
   passphrase goes over HTTPS to the setuid helper, which runs `flcrypt open -P -` with it on stdin
   and then mounts. **Built 2026-11-22:** Storage page card "Encrypted storage" (state, members
   open, passphrase box) → `POST /api/storage/unlock` (admins only, 5 failures per 5 min per user,
   then 429) → `flynas-helper cryptunlock <user>` with the passphrase on stdin (never on a
   command line) → `rc.d/flcrypt oneunlock` (exit 0 ok, 2 wrong passphrase, 3 too few members),
   logged to syslog as "storage unlock by <user>: ...". `GET /api/storage/encryption` reads
   `rc.d/flcrypt onestatus` (`state=unconfigured|locked|partial|unlocked`, members, open, need,
   mounted). The helper and rc.d paths are tested on the Pi as `nobody` (wrong and right
   passphrase, already mounted, partial, too few members, bad user name). The Lua endpoints and
   the card pass `dfly/tools/uitest/run-unlock.sh` on h2dev (2026-11-22; aes-xts there, the stock
   module): locked card, empty and wrong passphrase, 400 on a newline, unlock → "4 of 4 members
   open" and mounted, unlock while unlocked is a 200 no-op, 5 × 403 then 429. That run found the
   UI's frame arena overrunning the Clay context once the Storage page listed 15 disks (the dm
   mappings add four), fixed in `ui/main.c`.
4. `flcrypt open` unlocks the members in parallel (one process per member, so the four 1 s KDFs
   take about 1 s on the Pi's four cores) and exits 0 when all opened, 3 when some did, 2 when
   none did (wrong passphrase). The rc script mounts when at least `flcrypt_min` members are open,
   by default a majority (n/2+1, so 3 of 4): RAID6 keeps the data with two members gone, but
   HAMMER2's v3 header quorum needs a majority of headers to *mount* (§9.4).
5. Every attempt is logged by the caller (§5).

### 8.3a Shutdown

`rc.d/flcrypt stop` unmounts the array, then `flcrypt close -a` removes every mapping whose dm UUID
starts with `FLCRYPT-`. dm tables are RAM-only, so a power cut leaves nothing mapped.

### 8.4 FlyNAS's own secrets

Move the S3 secret keys and the cryfs password from the SD-card SQLite database onto the encrypted
array (a small file or SQLite database under the FlyNAS dataset), or wrap them with a key that is
stored on the array. Then they are only readable after unlock, which is also when backups can run.
Login data (accounts, TOTP secrets) stays on the root, since the unlock page needs it.

---

## 9. Operator workflow (commands checked against this tree)

### 9.1 Provisioning

```sh
# Writes both header copies on each slice and prints the recovery key once.
flcrypt format -r -L DATA /dev/da0s1 /dev/da1s1 /dev/da2s1 /dev/da3s1
flcrypt open /dev/da0s1 /dev/da1s1 /dev/da2s1 /dev/da3s1      # -> /dev/mapper/h2c0..3
newfs_hammer2 -R 6 -L DATA /dev/mapper/h2c0 /dev/mapper/h2c1 /dev/mapper/h2c2 /dev/mapper/h2c3
mount -t hammer2 /dev/mapper/h2c0:/dev/mapper/h2c1:/dev/mapper/h2c2:/dev/mapper/h2c3@DATA /data
```

Defaults: `-c xchacha12,aes-adiantum-plain64 -s 4096 -t 1000` (cipher, sector size, KDF time in ms);
`-k FILE` adds a keyfile slot; `-P FILE` reads the passphrase from a file (`-` for stdin, which is
what the FlyNAS helper uses). `format` refuses a slice that already has a header unless `-f`.
Unlike `tcplay -c`, it does not fill the slice with random data first; to hide which areas were
never written, `dd if=/dev/zero of=/dev/mapper/h2cN bs=1m` after `open` writes Adiantum output
over the whole data area (about 16 GB / 200 MB/s = 80 s per 16 GB slice per core). RAID6 needs at
least 4 members. An existing unencrypted array has to be copied off, re-created, and copied back.

**From the FlyNAS UI (2026-11-22):** the Storage page's create form has an *Encrypt* toggle (shown
while no flcrypt array is configured; one per box) with two masked passphrase fields (16 to 1024
characters). `POST /api/volumes {encrypt, passphrase}` (admins only) runs `flynas-helper
cryptcreate <label> <disks>` with the passphrase on stdin: `flcrypt format -f -r` on the disks'
`/dev/serno` paths, `open`, `newfs_hammer2 -R 6` on the mappings, mount on `/data/<label>`, then
`/etc/rc.conf.d/flcrypt` (`flcrypt_devices`, `flcrypt_mount="<label> /data/<label>"`) and
`flcrypt_enable="YES"`; no fstab line. The cipher is Adiantum when the loaded `dm_target_crypt` has
`kern.dm_crypt` (the FlyNAS kernel), else AES-XTS with 512-byte sectors (a stock kernel, e.g.
h2dev). The reply carries the recovery key once; the page shows it in a card until the admin
dismisses it, and nothing stores it. Deleting the volume unmounts it, closes the mappings and
removes the config; the headers stay on the disks (a later create formats over them). Tested by
`dfly/tools/uitest/test-storage.mjs` on h2dev (XTS); the Adiantum choice checked on the Pi only as
far as the probe, since its disks hold r6pi.

### 9.2 Recovery kit

- The **recovery key** printed by `format -r` (or `addkey -r`), written down and kept off the box.
- `flcrypt backup /dev/daNs1 FILE` per member: the 4 KB header, useless without a passphrase,
  recovery key or keyfile. `flcrypt restore DEV FILE` puts it back (refused onto another member's
  header unless `-f`); `flcrypt repair DEV` rewrites a damaged or stale copy from the good one.
- Both copies of a member's header destroyed and no backup: that member is lost even with the
  passphrase, but RAID6 resilvers it like a dead disk. Header loss on one or two members costs a
  resilver, not data.
- Off DragonFly: build `flcrypt.c` on Linux (`cc flcrypt.c -lcrypto -ldevmapper`); Linux dm-crypt
  has the same Adiantum and `sector_size` options, so `flcrypt open` maps the members there too
  (untested so far; HAMMER2 itself still needs DragonFly to read the array).

### 9.3 Replacing a disk

```sh
# A header for the new disk, from a live member's: same array UUID, cipher and sector size,
# member index 2, a new member UUID and master key.
flcrypt join -m /dev/da0s1 -i 2 [-k keyfile] [-R recoveryfile] /dev/da2s1
flcrypt open /dev/da2s1                                    # -> /dev/mapper/h2c2
umount /data; mount -t hammer2 <all four h2c mappings>@DATA /data
hammer2 -s /data raid replace /dev/mapper/h2c2 /dev/mapper/h2c2
```

If the array was mounted without the old member, remount it with all members first (as in
group G5): the member left out is recorded as FAILED, the new disk has no HAMMER2 header and is
treated as failed too, and `raid replace` then accepts its own path. While still mounted without
it, `raid replace` answers "not a member of the array".

`join` checks every secret it is given against the live member before using it, and can only put
those secrets in the new header: the passphrase always, the keyfile (`-k`) and the existing
recovery key (`-R`, from the written copy) if given; otherwise `addkey` them afterwards. It refuses
a data area smaller than the members'. The resilver writes plaintext to the mapping and ciphertext
reaches the disk. `raid replace` refuses a device that overlaps a live member (cf3ed04); that check
runs on the dm device and should see through a mapping to the slice (E13).

### 9.4 Interaction with RAID6 features

- **Header quorum** (`volhdr_quorum.md`): HAMMER2 reads the volume headers through the mappings.
  An unmapped member is a missing member. A mount needs a majority of the headers (with 4 disks,
  3); with 2 of 4 it fails with "v3 quorum: no majority ... 2 disks seen of 4, need 3" (seen on
  the Pi 2026-11-22) when the two missing members are not marked FAILED in any header (a cold
  mount with two members unplugged or not unlocked). If they failed while the array was mounted,
  the headers record it and an n-2 mount needs nothing special. For the cold case,
  `sysctl vfs.hammer2.quorum_force=1` lets the next mount go ahead with n-2 members that all agree
  on the chosen TXG; the sysctl is cleared by that mount, the missing members are marked FAILED
  and need `raid replace` (oct-review §9 item 8, group G6). It is deliberately not automatic and
  the unlock page does not set it: if the missing pair was itself mounted alone earlier, the two
  halves have diverged and one half's writes are lost at the next full mount. An admin sets it
  by hand after checking that the missing disks are really gone.
- **512-byte probes:** with `sector_size:4096` a mount logs `dm_target_crypt_strategy: can't really
  handle bp->b_bcount = 512 at 0` once per member (a probe of sector 0 at the 512-byte DEV_BSIZE).
  The probe fails and the mount carries on; harmless but noisy.
- **Scrub, self-heal, resilver, `h2stripe_check`:** unchanged, all on plaintext. They cost cipher
  time on every byte they read or write.
- **Compression:** LZ4 happens above RAID6, encryption below it, so data is compressed before it
  is encrypted, as it must be. Compression also reduces the bytes that need encrypting.
- **`DISK_GONE` and USB detach:** the dm mapping holds the disk open. A detach under it needs a test
  (E11): the mapping has to fail its I/O so HAMMER2 marks the member failed, not hang.
- **TRIM** (not implemented yet): it would show which sectors are free. Acceptable for this threat model.

### 9.5 Passphrase and key rotation

`flcrypt passwd` replaces the slot that the old passphrase opened; `addkey -N|-Q|-K|-r` adds a
passphrase, keyfile or recovery key in a free slot; `delkey -s N` removes one (refused for the
last slot, and for the slot that authorised the command unless `-f`; every member is checked
before any is changed). The master keys stay, so no data is rewritten (Google's KEK rotation,
§4.3). Rotating a master key itself is a re-create plus `raid replace` of each member in turn,
with the array degraded by one disk during each pass. Document it; don't automate it.

---

## 10. Tests and phasing

| # | Test | Pass condition |
|---|---|---|
| E1 | create, map, newfs, write, unmap, remap, read | identical bytes |
| E2 | 10 reboots with the rc.d unlock (and the FlyNAS unlock page) | every boot mounts clean |
| E3 | wrong passphrase | every map fails; nothing mounts; attempt logged |
| E4 | keyfile removed while mounted | the mount keeps working; the next unlock fails |
| E5 | one member's header destroyed, restored from `flcrypt backup` | member maps; parity check clean |
| E6 | one and two members left locked | mounts degraded; resilver after unlock |
| E7 | replace a member with a new encrypted disk | resilver; scrub and parity check clean |
| E8 | scrub on encrypted vs unencrypted array | same results; time ratio recorded |
| E9 | passphrase change (`flcrypt passwd`) on all members | new passphrase works, old fails, data intact |
| E10 | LZ4 under encryption | compression ratios unchanged |
| E11 | USB unplug of an encrypted member under write (Pi, user present) | member failed, no hang; replug and resilver |
| E12 | unlock time per member on the Pi, serial and parallel | recorded |
| E13 | `raid replace` onto a mapping of a live member's slice | refused |
| E14 | the full `tests/v3` suite with `DISK_LIST=/dev/mapper/h2c0 ...` | same pass count as unencrypted |

**flcrypt tests (2026-11-22, kernel #57).** `test_hdr.sh` (files, no dm): host 66/0, Pi 61/0 (the
five tamper tests need python3, which the Pi lacks). `test_dm.sh` on da0s1-da3s1 with HAMMER2 RAID6
on top: 37/0. It covers E1, E3 and E12, most of E5 (a zeroed primary header still opens; restore is in
`test_hdr.sh`), E6 for one member, E7 (`join`, open, remount, `raid replace`, then `h2stripe_check`
clean and the file read back without member 0), and the rc.d locked, keyfile and stop paths. On
the Pi, `format` calibrated 236k PBKDF2-SHA512 iterations for 1 s and took 5 s for four members;
`open` of four members takes 1-2 s (one process per member).

**Pi end to end (2026-11-23, kernels #62 and #63).** On da0s1-da3s1 (r6pi's slices, so r6pi is gone):
`test_hdr.sh` 61/0 (tamper tests skipped, no python3), `test_dm.sh` 37/0 three times. The FlyNAS
helper path, which the UI's create, unlock and delete call, run as root (no FlyNAS install on the Pi):
`cryptcreate` refuses a short passphrase, 3 disks and a second array; creates in 6 s with Adiantum
(`xchacha12,aes-adiantum-plain64`, `sector_size:4096`), members by `/dev/serno`, no fstab line;
`cryptstatus`, `onestop`, wrong passphrase (exit 2, nothing mapped), recovery key and passphrase both
unlock in 1 s with the data intact; after a reboot the array comes up locked and the passphrase
unlocks it; `voldestroy` leaves no config, mapping, rc.conf entry or mountpoint (headers stay) (19/0
plus the reboot and delete checks). The browser UI test itself still runs only on h2dev.

One `test_dm.sh` run on #62 crashed the Pi at `close -a` after the resilver: a NULL dereference in
`devstat_remove_entry` from `dm_dev_destroy`. The devstat list had no lock, and `flcrypt open`
creates its mappings from four processes at once through dm's MPSAFE ioctl, so an insert could be
lost and its later remove walked off the list (a parallel open / close loop lost one at round 42).
Fixed in the fork (35fd1697c1, a spinlock in `subr_devstat.c`, kernel #63): 150 rounds clean.

E14 is cheap: `DISK_LIST` (5eb700f) already names the members explicitly, and nothing in the suite
should care what kind of device a member is. The exceptions to check: group P (devstat is per
physical disk) and tests that write to member media directly to corrupt it (they would corrupt
ciphertext, which is fine: it decrypts to garbage, which is what they want).

**Phasing:**
1. **A (h2dev and Pi):** E1, E3, E4, E12, E14. Build `dm`/`dm_target_crypt`/`crypto` as part of the
   Pi kernel deploy (`pi-h2deploy.sh`) instead of by hand.
2. **B:** the rc.d script and the FlyNAS unlock page and helper verb (runs `flcrypt open -P -`); E2, E6. Move the
   FlyNAS secrets (§8.4).
3. **C:** E5, E7, E11, E13.
4. **D:** E8, E9, E10; operator runbook (recovery kit, rotation).
5. **Adiantum (chosen, §7 item 3):** `sector_size:4096` and the pipeline fixes are done (§6.7);
   the FlyNAS header and command-line helper (`flcrypt`, §8) and its rc.d script are done and
   tested (above). Still to do: the FlyNAS unlock page and helper verb (phase B, calling `flcrypt
   open -P -`), E2, E4, E8-E11, E13, E14, the x86 build check (h2dev was down), and committing the
   `dm_target_crypt`/crypto changes, `spread_strategy` and flcrypt.
   **Later:** native encryption (§3.4).

---

## 11. Changes from the May 2026 draft

- Target hardware is the Pi 4 (no AES instructions), not a Ryzen with AES-NI. The old performance
  section (3-5 GB/s per core, 5-10% overhead) does not apply; §6 has measurements.
- Encrypting "above RAID6" was never possible: HAMMER2's RAID6 has no array device.
- Minimum array is 4 disks, not 3.
- Commands: `newfs_hammer2 -R 6 ...` (not `--raid6`); `mount -t hammer2 dev:dev:...@LABEL`;
  `hammer2 -s MNT raid replace N DEV` (not `hammer2 resilver`); tcplay PRF `SHA512-VC` (the draft's
  `SHA-512` is not a valid name, and plain `SHA512` is the 1,000-iteration TrueCrypt format);
  header backups with `--modify --save-hdr-to-file` (there is no `--save-hdr`).
- tcplay headers have one passphrase slot (plus keyfiles), so recovery works through the printed
  passphrase and header backups, not a second key slot.
- Added: the Seafile analysis (§2, §3.1), the guest-LUKS and native options, Google's design (§4-5),
  the FlyNAS unlock flow (§8.2), the FlyNAS secrets (§8.4), tests E12-E14.

---

## Sources

- Google Cloud, *Default encryption at rest*:
  <https://docs.cloud.google.com/security/encryption/default-encryption>
- Google Cloud, *Cloud Key Management Service deep dive*:
  <https://docs.cloud.google.com/security/key-management-deep-dive>
- Google Cloud, *Encryption at rest* whitepaper (PDF):
  <https://cloud.google.com/security/encryption-at-rest/resources/encryption-whitepaper.pdf>
- Google Cloud, *Envelope encryption*: <https://docs.cloud.google.com/kms/docs/data-encryption-keys>
- DragonFly sources in this tree: `sys/dev/disk/dm/crypt/dm_target_crypt.c`, `lib/libtcplay/`.
