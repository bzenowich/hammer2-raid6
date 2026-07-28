# Bitrot: What the Public Field Data Actually Says

*Compiled 2026-07-28. All figures are from publicly released papers, datasets, or vendor
disclosures; links in [Sources](#sources).*

---

## 1. First, a correction on the premise

It is worth being precise about what Google and Backblaze actually published, because it
is not bitrot:

- **Backblaze Drive Stats** is a raw, downloadable dataset (daily SMART snapshots of every
  drive in their fleet, continuously since 10 April 2013, ~349,000 drives as of the 2025
  report). It measures **drive failure and SMART attributes** — not silent corruption.
  Backblaze has never published a silent-data-corruption rate. Their integrity protection
  (per-shard checksums in their Reed-Solomon vaults) exists, but the shard-repair counts
  are not published.
- **Google** published four influential *papers*, no raw datasets: disk failures
  (FAST '07, >100,000 drives), DRAM errors (SIGMETRICS '09), flash (FAST '16), and CPU
  silent corruption (HotOS '21). Only the last two touch bitrot proper, and Google
  explicitly declined to disclose exact CPU error rates "for business reasons."

So the honest framing is: **Backblaze gives you the only large, open, raw storage-failure
dataset; Google gives you papers. The best actual bitrot numbers come from other
organizations** — chiefly CERN, NetApp, Meta, and Alibaba.

## 2. What "bitrot" means, layer by layer

The word is used for at least five distinct phenomena with error rates differing by
orders of magnitude. Mixing them is the most common mistake in this literature.

| Layer | Failure mode | Detected by |
|---|---|---|
| Media (platter/cell) | Latent sector errors, uncorrectable read errors | Drive ECC, scrubbing |
| Drive/controller/firmware | Lost writes, misdirected writes, torn writes | Block checksums, identity metadata |
| Storage stack | Corruption injected by HBAs, cables, drivers, RAID | End-to-end checksums (ZFS, WAFL) |
| Memory (DRAM/HBM) | Bit flips, hard cell faults | ECC (single-bit correct, double-bit detect) |
| Compute (CPU/GPU) | Corrupt execution errors — wrong arithmetic, silently | Almost nothing; only redundant execution |

Only the top three are "bitrot" in the classical archival sense. The bottom two are what
the industry now calls **SDC (silent data corruption)**, and they are where all the
recent research energy is.

---

## 3. Who else has released public bitrot data

### 3.1 CERN — the canonical silent-corruption measurement (2007)

Still the most-cited real bitrot measurement, and the only one that instrumented every
layer at once. Bernd Panzer-Steindel's internal report was released publicly, with
follow-up presented by Peter Kelemen.

| Probe | Method | Result |
|---|---|---|
| Disk write/read-back | 2 GB patterned file, every 2 h, 3,000+ nodes, 5 weeks | **500 errors on 100 nodes** |
| — error breakdown | | 10% single-bit, 10% sector/page-sized, 80% 64 KB regions |
| RAID-5 verify | 492 systems (~1.5 PB), 4 weeks | **~300 bad blocks fixed**; ~850 expected from the vendor 1-in-10¹⁴ BER |
| Memory | ~1,300 nodes, ~3 months | 44 errors reported (41 ECC-correctable, **3 double-bit**) |
| CASTOR checksum verify | 33,700 files (8.7 TB) vs. tape Adler-32 | **22 mismatches — 1 bad file in 1,500**, byte error rate ~3×10⁻⁷ |
| Follow-up (Kelemen) | ~97 PB written over 6 months | **~128 MB permanently, silently corrupted** (~1.3×10⁻⁹) |

Headline conclusion: *"low level data corruptions exist… error rates are at the 10⁻⁷
level, but with complicated patterns."* Two details are routinely omitted when this study
is cited:

1. The **disk** error rate was roughly *in line with* (in fact slightly better than)
   vendor BER specs — 300 observed vs. 850 expected. The disks were not the scandal.
2. The 80% of errors that were 64 KB regions were traced to a **firmware bug** (WD drives
   dropping out of 3ware RAID controllers). Most "bitrot" at scale is a bug, not physics.
3. A brutal aside for archivists: for **compressed** files, a single bit error rendered
   the whole file unreadable with 99.8% probability.

### 3.2 NetApp — the largest storage-corruption study ever published (2007–2008)

Bairavasundaram et al., using logs from tens of thousands of production NetApp filers.
Two papers, **1.53 million drives**, 41 months. This is the gold standard for
disk-and-below corruption rates.

**Checksum mismatches (FAST '08)** — >400,000 instances observed:

| Metric | Nearline (SATA) | Enterprise (FC) |
|---|---|---|
| Drives developing ≥1 mismatch in first 17 months | **0.66%** | **0.06%** |
| Over the whole study | 3,088 / 358,000 = 0.86% | 767 / 1.17 M = 0.066% |
| % of disks affected per year (avg) | 0.466% | 0.042% |
| Worst individual drive model | up to **4%** in 17 months | — |

Other findings:
- **8% of all checksum mismatches were discovered during RAID reconstruction** — i.e. at
  the exact moment redundancy was gone. This is the real data-loss path.
- Mean mismatches per *corrupt* disk: **104; median 3**. A tiny number of sick drives
  produce nearly all the corruption. Counterintuitively, enterprise drives corrupt *less
  often* but produce *more* errors once they start.
- Errors are **not independent** — strong spatial locality (consecutive blocks), strong
  temporal clustering, and correlation between different disks in the same system.
- **Scrubbing found 49% of nearline and 73% of enterprise mismatches.** Without scrubbing,
  half of them would have surfaced only on a read or a rebuild.
- Specific **block numbers** were disproportionately corrupted across unrelated systems —
  strong evidence of firmware bugs. Recommendation: stagger RAID stripes so a stripe does
  not use the same LBA on every disk.
- Rarer sibling classes: **identity discrepancies** (lost/misdirected writes) hit only
  365 of 1.53 M drives; **parity inconsistencies** were 3.5–4.4× rarer than checksum
  mismatches.

**Latent sector errors (SIGMETRICS '07)**, same population, 32 months. LSEs are an order
of magnitude more common than silent corruption, but far less dangerous because the drive
*reports* them:

- **3.45%** of all 1.53 M disks developed at least one LSE over 32 months — **8.5% of
  nearline** vs. **1.9% of enterprise** drives.
- Age-controlled, within 12 months of ship date: **3.15% nearline, 1.46% enterprise**.
  Expressed as the FAST '08 companion table does — % of disks affected per year — that is
  **9.5% nearline vs. 1.4% enterprise**.
- Model variance swamps class variance: at 24 months, the fraction of affected nearline
  disks ranged from **5% to 20%** depending on model.
- Affected fraction grows **linearly with time for enterprise, super-linearly for
  nearline**, and rises with drive capacity. Annual sector error rate jumps sharply
  between years one and two.
- **>60% of LSEs were discovered by scrubbing** (vs. 49–73% for checksum mismatches).
- Highly skewed: >80% of error disks have fewer than 50 errors, ~37–39% have exactly one,
  and 0.2% of error disks had >1,000 and had to be excluded as outliers.
- Same non-independence as corruption: a disk with one LSE is markedly more likely to
  develop more, with strong spatial and temporal locality (~10 MB locality radius, vs.
  literally consecutive blocks for checksum mismatches — evidence the two have different
  physical causes).

### 3.3 Meta / Facebook — CPU silent data corruption (2021–)

Meta broke the industry silence with two papers plus an engineering blog post.

- Fleet: hundreds of thousands of machines; **"hundreds of CPUs detected for SDCs."**
- Stated occurrence rate: **~1 in 1,000 silicon devices** — "reflective of fundamental
  silicon challenges."
- These are **not soft errors**: they are systemic, repeatable, defect-driven, and
  data-dependent. The canonical example — computing `(1.1)^53` returned `0` instead of
  `156.24` on one core, silently dropping database rows.
- Detection at scale requires two complementary mechanisms:
  - **Fleetscanner** (out-of-production testing): 68 M lifetime test iterations, 4 billion
    fleet-seconds, **93% coverage** of detected SDCs, full-fleet sweep in **5–6 months**.
  - **Ripple** (opportunistic in-production testing): 2.5 B test instances/month, detects
    **70% of common failures within 15 days**, and catches 7% that Fleetscanner cannot.

Meta also published DRAM field data (DSN '15, 14 months, entire fleet): **9.62% of servers
experienced correctable memory errors** over 12 months — down from Google's 32.2% seven
years earlier — with errors following a **Pareto distribution** (mean exceeds median by
~55×) and a large share attributable to sockets/channels rather than cells.

### 3.4 Alibaba Cloud — the most precise public SDC numbers (SOSP '23)

Tsinghua + Ohio State + Alibaba Cloud, **over one million processors**. Because they
disclosed exact rates, this is now the reference point.

| Measure | Rate |
|---|---|
| CPUs identified as causing SDC (overall) | **3.61‱** (≈ 1 in 2,770) |
| Found in pre-production testing | 3.262‱ (90.4% of all faulty CPUs) |
| Found in regular in-production testing | 0.348‱ |
| Range across 9 micro-architectures | 0.082‱ – 9.29‱ |

- **Failure rates do not improve with newer chips.** Testing gets better, but so does
  complexity.
- Five vulnerable feature families: arithmetic logic, vector ops, floating point, cache
  coherency, transactional memory.
- **~Half of faulty processors had only one bad physical core** — notably more multi-core
  defects than Google reported.
- On floating-point corruption, bit flips **predominantly hit low-order bits**, rarely the
  most significant — corruption is often plausible-looking, not obviously wrong.
- **Temperature is a trigger**: for less-reproducible SDCs, occurrence frequency grows
  roughly **exponentially with core temperature**, even within spec. Some defects only
  fire above ~59 °C. This is the basis of their "Farron" mitigation (prioritized testing
  plus thermal control).

### 3.5 Google — flash and CPUs

**Flash (FAST '16)**, 6 years, millions of drive days, 10 models:

- **20–63% of drives experience at least one final (uncorrectable) read error**, depending
  on model; uncorrectable errors affect **2–6 of every 1,000 drive days**.
- Median RBER ranges 5.4×10⁻⁹ to 3.2×10⁻⁸ across models; 99th-percentile drives are
  ~an order of magnitude worse than the median of the same model.
- **RBER is a poor predictor of uncorrectable errors**, and it grows far more slowly with
  wear than the exponential model everyone assumed. UBER (errors per bit read) is
  characterized as a misleading metric.
- **SLC is not more reliable than MLC** in the field.
- Versus HDDs: lower replacement rates, but **higher uncorrectable error rates**. Flash
  trades drive death for data loss.

**CPUs (HotOS '21, "Cores that don't count")**: coined "mercurial cores." Rate disclosed
only as **"a few mercurial cores per several thousand machines"** — consistent with Meta.
Key findings: defects are per-core rather than per-chip, appear long after installation,
are frequency/voltage/temperature-sensitive, and can be exposed suddenly by innocuous
software changes that increase use of a rare instruction.

**Disks (FAST '07)**, >100,000 drives — included here because it is the standard companion
citation: SMART is a weak predictor. After a first scan error a drive is **39× more likely
to fail within 60 days**, yet **56% of failed drives had no counts in any of the four
strongly-predictive SMART signals**, and **36% had zero counts across all SMART
variables**. Temperature and utilization correlated far less than folklore claims.

### 3.6 Microsoft — SSD failures in datacenters (SYSTOR '16)

Over **half a million SSDs** in Azure/Bing infrastructure. Characterized how SSD failures
manifest in production — symptom classes, provisioning and operational factors — rather
than corruption rates specifically. Paper public, data not.

### 3.7 NetApp again — enterprise SSDs (FAST '20, FAST '22)

**~1.4 million enterprise SSDs.** Notable: an unexpectedly long infant-mortality period
(drives spend 20–40% of life in it), wear-out is essentially a non-issue (most drives
consume <1% of rated life; even heavily used ones only 15–33%), and **firmware bugs
dominate the interesting failure classes**, including lost writes.

### 3.8 HPC and GPU memory

- Field studies exist for **Titan (K20X)**, **Blue Waters (K20X)**, and **Summit (V100)**
  GPU memory error logs, plus recent Ampere-generation work. ECC catches single-bit
  errors; double-bit errors are detected-but-fatal, and undetected corruption remains.
- Fault-injection work consistently shows GPUs are far more SDC-prone than CPUs per
  injected fault: **16–33% of injected faults produce SDC in GPU HPC codes vs. <2.3% for
  CPU codes**.
- **LANL** released 9 years of failure data (22 systems, 4,750 nodes, 24,101 processors,
  1996–2005) through the **USENIX CFDR**, including disk replacement records — the
  original public failure-data release, predating Backblaze by seven years.

### 3.9 The AI-era data (2024–2026)

This is where the field has moved, and where the newest public numbers are.

- **Meta's Llama 3 training disclosure** (16,384 H100s, 54 days) is effectively a public
  reliability dataset: **466 job interruptions, 419 unexpected**, roughly one failure every
  3 hours. GPUs caused 30.1%, HBM3 memory 17.2%, GPU SRAM 4.5%, GPU processor 4.1% — and
  **SDC accounted for ~1.4% of unexpected interruptions**. Only 2 CPU failures in 54 days.
- **OCP Server Component Resilience workstream** — Meta, Google, Microsoft, Intel, AMD,
  Arm, NVIDIA jointly working on SDC, with open-sourced tooling (Intel/Arm Open Datacenter
  Diagnostics, AMD Open Field Health Check, NVIDIA DCGM) and an SDC-in-AI whitepaper. A
  joint IEEE Micro (Jan 2026) call-to-action followed.
- The framing that has emerged: at ~1 SDC-affected machine per 1,000 and tens of thousands
  of accelerators per training run, **large training jobs should expect SDC events on the
  order of every one to two weeks**, and a corrupted gradient propagates to every replica
  before anything crashes.
- Academic follow-ups now measure SDC's effect on training directly (AWS/ACL 2025 compared
  identical training on healthy vs. known-unhealthy nodes; TU Berlin 2026), finding SDC is
  **non-uniform over time and node-specific**, and can perturb individual submodule outputs
  by large factors.

---

## 4. Raw datasets you can actually download

Papers are plentiful; open raw data is rare. The complete list, as best as it can be
established:

| Dataset | Scale | Period | Notes |
|---|---|---|---|
| **Backblaze Drive Stats** | ~350k drives, daily | 2013–present | Daily SMART CSVs; the only continuously updated one |
| **Alibaba `dcbrain` SSD SMART logs** | ~500k SSDs, 6 models | 2018–2019 | Released with DSN '21; larger ~1 M-drive variant with FAST '21 |
| **Alibaba Tianchi disk-failure dataset** | large-scale HDD | — | Competition release |
| **Baidu SMART dataset** | 25,792 drives, 3 models | 2016 | 12 normalized features |
| **SMART-Z** (Nature *Scientific Data*, 2025) | 147,496 disks, 65 models, 712 failures | Mar 2017 – Feb 2018 | ZTE Corp. + Tianjin Univ. of Technology + Peking Univ.; distributed video datacenter. Adds critical/worst values, device IP, business scenario, drive letter, and power-on time for offline disks. 5.3% blank fields vs. Backblaze's 14.8%. On OSF, [doi:10.17605/OSF.IO/24Y6G](https://doi.org/10.17605/OSF.IO/24Y6G), CC-BY 4.0 |
| **USENIX CFDR** (LANL, PNNL, HPC clusters) | 4,750 nodes / 24k CPUs + disk replacements | 1996–2005 | Registration required |
| **UCSD/Murray** | small | ~2005 | 60 SMART attributes; historical |

Every one of these is a **failure/SMART** dataset. **No organization has published a raw
silent-corruption event dataset.** The corruption numbers in Section 3 exist only as
aggregate statistics inside papers.

---

## 5. What all the evidence agrees on

1. **Bitrot is real but rare per-device, and unavoidable per-fleet.** Sub-1%-per-year at
   the drive level; ~0.1% of CPUs; but at 10⁵–10⁶ devices it is a daily event.
2. **Firmware and hardware defects dominate cosmic rays.** CERN traced 80% of errors to a
   firmware bug; NetApp found block-number-specific corruption across unrelated systems;
   Meta and Alibaba both concluded CPU SDC is systemic manufacturing defect, not soft
   error. Google's DRAM study likewise found **hard errors dominate soft errors** — the
   single most-overturned assumption in the field.
3. **Errors are clustered, not Poisson.** Spatial locality within a disk, temporal
   clustering, correlation between disks in an enclosure, Pareto-distributed memory errors
   (Meta: mean/median ≈ 55×), a few sick devices producing most events. Reliability models
   assuming independence are wrong in the dangerous direction.
4. **Detection is the entire game.** The only errors that cause data loss are the ones
   nothing checks. Scrubbing caught ~half to three-quarters of NetApp's mismatches;
   8% surfaced only during RAID reconstruction, when redundancy was already gone.
5. **End-to-end checksums are the only real defense** — every study, from CERN's
   "checksum mechanisms have to be implemented and deployed everywhere" to S3's
   per-shard checksums validated continuously across the fleet, reaches the same
   conclusion. Note the ZFS caveat (FAST '10): ZFS is robust against disk corruption but
   **not** against memory corruption, because the checksum is computed after the data is
   already in RAM.
6. **Enterprise ≠ immune, and newer ≠ better.** Enterprise drives corrupt ~10× less often
   but worse when they do; SLC is not more reliable than MLC; CPU SDC rates are not
   improving generation over generation.
7. **Vendor specs are roughly right for media, useless for everything else.** CERN's disk
   measurement landed within 3× of the spec BER, but nothing in the spec sheet predicts
   firmware bugs, lost writes, bad HBAs, or mercurial cores.
8. **Compression and dedup amplify bitrot catastrophically.** CERN: one bit flip destroys
   a compressed file 99.8% of the time.

For reference, vendor uncorrectable-bit-error specs: consumer SATA HDD ~1 in 10¹⁴,
enterprise HDD ~1 in 10¹⁵–10¹⁶, LTO-9 tape ~1 in 10²⁰. Tape's spec advantage is real and
large, but it is a *media* number and says nothing about the controller, memory, and CPU
layers where most measured corruption actually originates.

## 6. Notable gaps

- **No cloud provider publishes corruption rates.** AWS, Azure, and GCP publish durability
  *targets* (11 nines) and describe their mechanisms, but no measured silent-corruption
  incidence. Amazon's most substantive public statement remains the 2019 re:Invent talk on
  S3's durability culture, which describes continuous fleet-wide checksum validation and
  erasure-coded shard repair without giving numbers.
- **Drive manufacturers publish nothing** from field-return data.
- **Backblaze has the raw pipeline to measure this and does not report it** — shard
  integrity failures would be the single most valuable missing dataset in this whole area.
- **Consumer/archival storage is unmeasured.** All numbers here come from datacenters with
  active scrubbing. Cold disks in a closet are unstudied at scale; the digital-preservation
  community works from fixity-check anecdotes and the 2007 Internet Archive figure of ~8%
  annual drive failure over 1,000 drive-years.

---

## 7. Detecting and correcting bitrot with hammer2-raid6

Everything above is the threat model. This section is what this implementation
actually does about it, what it does *not* do, and how to operate it.

### 7.1 The detection model: checksums localize, parity repairs

The single most important structural fact is that **P/Q parity cannot detect
bitrot on its own, and cannot identify the guilty column even when it does.** A
RAID6 syndrome mismatch tells you a row is inconsistent; it does not tell you
whether the data column, P, or Q is the liar. Classic RAID6 arrays that "fix"
this by majority-voting the syndrome are guessing.

hammer2-raid6 splits the job the way ZFS does:

| Step | Mechanism |
|---|---|
| **Detect** | Per-block CHECK code stored in the *parent* blockref (Merkle tree) |
| **Localize** | `bref.copyid` names the disk holding that block's column |
| **Repair** | P/Q reconstruction of that column, treating the named disk as failed |
| **Confirm** | Re-verify the reconstructed bytes against the same CHECK code before committing |

The CHECK code is what makes the parity usable. It converts a *corruption*
(unknown-position error, which RAID6 tolerates only one of) into an *erasure*
(known-position error, of which RAID6 tolerates two).

Verification lives in `hammer2_chain_testcheck()`
(`src/sys/local_hammer2_chain.c:5898`) on the normal read path and in
`hammer2_scrub_check_match()` (`src/sys/local_hammer2_io.c:2024`) for scrub.
Supported methods are `HAMMER2_CHECK_ISCSI32` (CRC32C),
`HAMMER2_CHECK_XXHASH64`, `HAMMER2_CHECK_SHA192`, and `HAMMER2_CHECK_FREEMAP`
(icrc32). `newfs_hammer2` defaults every blockref to **XXHASH64**
(`src/sbin/local_mkfs_hammer2.c:144`); per-file/per-tree overrides come from
`hammer2 setcheck <none|crc32|xxhash64|sha192> <path>`.

Mapped against §2's five layers:

| Layer | Covered? | By what |
|---|---|---|
| Media (LSE, URE) | Yes | Drive reports EIO → `hammer2_raid6_auto_fail_disk()`, then P/Q reconstruction |
| Drive/controller/firmware (lost, misdirected, torn writes) | Yes | Blockref CHECK code — the checksum lives in the *parent*, so a stale or misplaced block fails verification even though it is internally self-consistent |
| Storage stack (HBA, cable, driver) | Yes | Same end-to-end CHECK code, computed above the block layer |
| Memory | **No** | Checksum is computed after data is already in RAM — the FAST '10 ZFS caveat from §5.5 applies verbatim |
| Compute (mercurial cores) | **No** | Nothing here defends against a CPU that computes the wrong XXH64 |

### 7.2 Detecting it: `hammer2 raid scrub`

```
hammer2 -s <mnt> raid scrub        # blocking; prints final counters
hammer2 -s <mnt> raid scrub -n     # forks the blocking ioctl, polls progress once a second
```

`raid scrub` (`src/sbin/local_cmd_raid.c`) issues `HAMMER2IOC_RAID_SCRUB`, which
runs `hammer2_io_raid6_scrub()` (`src/sys/local_hammer2_io.c:2255`) in the
kernel. The walker:

- takes a `hammer2_chain_bulksnap()` of `hmp->vchain`, so it scrubs the state as
  of the last TXG flush;
- recurses only into interior bref types (INODE / INDIRECT / VOLUME / FREEMAP /
  FREEMAP_NODE) and verifies DATA / DIRENT leaves in place;
- locks chains SHARED with `HAMMER2_LOOKUP_NODATA | HAMMER2_LOOKUP_SHARED` and
  uses bulkfree's unlock-recurse-relock pattern, so writers needing EXCLUSIVE on
  an already-walked chain do not block behind the scrub;
- serializes against itself with `atomic_cmpset_int()` on `hmp->scrub_running` —
  a second concurrent scrub gets `EBUSY`.

It is **not a background daemon**: the walk runs inside the blocking ioctl in the
calling process, and nothing in the tree schedules it. Scrubbing is something you
arrange (§7.6), not something that happens.

One sharp edge: the walker locks each parent `RESOLVE_ALWAYS | RESOLVE_SHARED`,
so interior chains *are* checksum-verified in passing — but a parent carrying
`HAMMER2_ERROR_CHECK` causes the walker to return immediately
(`local_hammer2_io.c:2189-2192`, `:2225`). **A corrupt INDIRECT block silently
removes its entire subtree from the scrub**, and the event is counted in neither
`brefs_done` nor `brefs_bad`. A scrub that reports far fewer `brefs_done` than
the previous run is the only signal.

**Scrub runs concurrently with a live workload.** Group K's K3 test writes in the
foreground (0–1 s completions on the vbd substrate) while a 32 MB scrub runs to
completion. The tradeoff is that chains created since the last TXG sync are not
covered by the current scrub; the next one picks them up.

Reported counters, both on stdout and via the non-blocking
`HAMMER2IOC_RAID_SCRUB_STATUS` poll:

| Counter | Meaning |
|---|---|
| `brefs_done` | DATA/DIRENT blockrefs verified |
| `brefs_bad` | CHECK code mismatches found (or unreadable) |
| `brefs_repaired` | Mismatches successfully rebuilt from P/Q and written back |
| `brefs_unrepairable` | Mismatches parity could not fix — **actual data loss** |

`hammer2 raid scrub` exits non-zero if `error` or `brefs_unrepairable` is
non-zero, so it drops straight into cron/monitoring without parsing.

A chain whose primary disk is currently *failed* passes scrub rather than
producing a false positive: the read is serviced by
`hammer2_io_raid6_read_degraded()` inside `hammer2_io_bread()`, so what gets
checksummed is the reconstruction.

### 7.3 Correcting it

On a CHECK mismatch, `hammer2_scrub_verify_bref()`
(`src/sys/local_hammer2_io.c:2053`) does the following:

1. Releases the read-side dio (`hammer2_io_putblk`) — required before any
   write-side `getblk()` on the same `(devvp, phys_off)`, or the second `getblk`
   deadlocks on the first's busy buf.
2. Calls `hammer2_io_raid6_read_degraded(..., disk_idx = bref->copyid, ...)`,
   reconstructing the entire `stripe_unit` column from the surviving columns plus
   P/Q. Reconstruction math is Reed-Solomon over GF(2⁸) with polynomial `0x11d`
   (`src/sys/local_hammer2_raid6.c`): `P = XOR(data)`, `Q = Σ data[i]·2ⁱ`, with
   `hammer2_raid6_dual_recov()` for two lost columns and
   `hammer2_raid6_datap_recov()` for data+P.
3. **Re-runs `hammer2_scrub_check_match()` on the reconstructed bytes.** If the
   reconstruction does not itself verify, nothing is written. This matters: a
   silently-corrupt P or Q would otherwise let a "repair" overwrite good data
   with garbage.
4. Only then writes the column back with a raw `getblk` + `bwrite` directly on
   `hmp->volumes[disk_idx].dev->devvp`, deliberately bypassing
   `hammer2_io_bwrite()`. Under v3 the DIO write path hands DATA/DIRENT payloads
   to the packed-open-row tracker (`hammer2_raid6_open_row_add_data`); a repair
   routed through it would open a fresh row with `ncols=1` and zero the surviving
   sibling data columns at seal time — corrupting the very row it was fixing.

Success and failure are both logged:

```
hammer2: scrub: repaired data_off <off> disk <n>
hammer2: scrub: CHECK FAIL data_off <off> disk <n> (parity could not repair, err=<e>)
hammer2: scrub complete: <N> brefs, <N> bad, <N> repaired, <N> unrepairable, err=<e>
```

Media errors are handled separately from silent corruption: an EIO from any
column during reconstruction or resilver triggers
`hammer2_raid6_auto_fail_disk()` (`src/sys/local_hammer2_io.c:1344`), which marks
the disk failed in both `hmp->raid_failed[]` and the on-disk
`voldata.raid_config` so the state survives remount, logging:

```
hammer2: RAID6 disk <n> auto-failed due to I/O error
hammer2: RAID6 unrecoverable: I/O error on disk <n> but already <k> disk(s) failed
```

### 7.4 What is *not* automatically corrected

Stating these plainly matters more than the feature list, because §3.2's
finding — 8% of NetApp's checksum mismatches surfaced during RAID reconstruction,
when redundancy was already gone — is exactly the scenario these gaps feed.

- **There is no self-heal on the read path.** When `hammer2_chain_testcheck()`
  fails on a normal read, `hammer2_chain_load_data()` sets
  `chain->error = HAMMER2_ERROR_CHECK` (`src/sys/local_hammer2_chain.c:1279`) and
  the error propagates to the caller. It does **not** trigger parity
  reconstruction. Read-path reconstruction fires only when the disk is already
  marked failed in `raid_failed[]`, not on a checksum mismatch from an ONLINE
  disk. ZFS repairs inline here; this implementation does not. **Consequence: a
  latent corruption is fixed only when a scrub reaches it, so scrub cadence is
  load-bearing, not hygiene.**
- **Scrub *repairs* DATA and DIRENT only.** `hammer2_scrub_verify_bref()` returns
  immediately for every other bref type. INODE, INDIRECT, FREEMAP_NODE and
  FREEMAP_LEAF live in the N-way-mirrored metadata zone (`docs/metadata_zone.md`);
  they get checksum-verified incidentally by the walker's `RESOLVE_ALWAYS` lock,
  but a failure is neither counted nor repaired — it just prunes the subtree.
  Their mirror copies are consulted by `hammer2_io_metadata_mirror_read()`
  (`src/sys/local_hammer2_raid6.c:574`) only when the primary disk is *failed*,
  never on CHECK mismatch, even though `docs/metadata_zone.md:175-177` specifies
  check-aware failover. **Metadata bitrot is detected on access and reported, but
  no code path repairs it.**
- **Resilver does not verify checksums.** Neither the metadata-zone bulk copy
  (Phase A, `local_hammer2_io.c:1734`) nor the per-stripe rebuild
  (Phase 3, `:1844`) checks a CHECK code on the data it reads. A silently corrupt
  surviving column is faithfully reconstructed onto the replacement disk, and
  `raid replace` does not chain a scrub on completion. This is the mechanism
  behind §3.2's most dangerous statistic, applied to this implementation
  specifically.
- **Volume-header quorum can gate mount before RAID6 gives up.** Mount requires
  the newest TXG sequence durable on `(ndisks/2)+1` disks
  (`local_hammer2_ondisk.c:1020`). At N=6 that is 4, so three failed disks are
  unmountable even where RAID6 arithmetic could in principle still reconstruct.
  If the surviving majority is *behind*, mount rolls back and loses the
  intervening writes — refused by default, allowed only via
  `vfs.hammer2.j2_allow_rollback=1`, bounded by `vfs.hammer2.j2_rollback_max`
  (default 8 TXGs). See `docs/volhdr_quorum.md:126-139`.
- **The stripe bitmap lives on disk 0 only and is not mirrored.** Losing disk 0
  forces a full chain-walk rebuild at mount.
- **SHA192 blocks are skipped by scrub.** `hammer2_scrub_check_match()`'s
  `default:` arm returns "match" for check types it cannot compute in `io.c`, so
  a tree set to `hammer2 setcheck sha192` gets weaker scrub coverage than the
  XXHASH64 default. `CHECK_NONE` / `CHECK_DISABLED` return "match"
  unconditionally — no detection at all.
- **Memory and CPU corruption are out of scope**, per §7.1's table. Given §3.4's
  finding that floating-point SDC predominantly flips low-order bits — producing
  plausible-looking wrong answers — and §5.5's ZFS caveat, ECC RAM is not
  optional for an array holding data you care about.
- **No Dirty Time Log.** Item 5 in `docs/zfs_compare.md` is deferred, so every
  `raid replace` is a full resilver over all live slots rather than only the
  slots written while the disk was down.
- **Three failures is the wall.** `hammer2_raid6_auto_fail_disk()` returns ENXIO
  rather than failing a third disk. Two lost columns is the design limit, and
  a silent corruption discovered while already double-degraded is unrepairable
  by construction.

### 7.5 Offline checking: `h2stripe_check`

The online scrub verifies **data against its checksum**. It does not verify
**P/Q against the data** — a row whose data columns are all intact but whose
parity is stale or corrupt scrubs clean, and only bites later during a resilver.
That is NetApp's third corruption class (parity inconsistencies, §3.2), and it
gets its own tool:

```
h2stripe_check [-v] /dev/da0 /dev/da1 ... /dev/daN     # array must be UNMOUNTED
```

`src/diag/h2stripe_check.c` reads the physical stripe bitmap from zone slot 41 on
disk 0 and, for every allocated slot, reads all data columns, recomputes P (XOR)
and Q (GF(2⁸) syndrome), and compares against the on-disk P and Q. Exit 0 if
every checked stripe is consistent, 1 on any parity or read error.

`src/diag/h2parity_fix.c` also exists but is a narrow one-off: it is hardcoded to
a 4-disk `diskN.img` layout and was written to repair parity damaged by a
specific historical `GETBLK_NOWAIT` bug. It is not a general repair tool.

### 7.6 A defensible operating regimen

The field data in §3 gives the cadence its justification:

1. **Scrub on a schedule, monthly at minimum.** NetApp found scrubbing caught
   49% of nearline and 73% of enterprise checksum mismatches, and >60% of latent
   sector errors — i.e. roughly half of all corruption is discoverable *only* by
   scrubbing or by a read that may never come. Since §7.4 establishes there is no
   read-path self-heal here, that share is higher for this implementation than
   for ZFS.
   ```
   0 3 * * 0  hammer2 -s /mnt/tank raid scrub || logger -p daemon.err "hammer2 scrub found unrepairable blocks"
   ```
2. **Alert on `brefs_bad > 0`, not just on `brefs_unrepairable`.** §3.2's
   strongest operational finding is that errors are clustered, not Poisson: mean
   mismatches per corrupt disk was 104 with a median of 3, and a handful of sick
   drives produce nearly all corruption. The first repaired block on a given disk
   is a leading indicator, not a curiosity. `hammer2 raid scrub` only exits
   non-zero on unrepairable blocks, so watch the `brefs_repaired` /
   `brefs_bad` counters explicitly.
3. **Log every run's counters yourself.** The kernel fields are zeroed at the
   start of each scrub (`local_hammer2_io.c:2271-2275`) and do not survive
   unmount. There is no cumulative corruption count, no per-disk CKSUM column,
   and no sysctl exposing either — the `zpool status` equivalent does not exist.
   A per-run history file is the only way to see the trend that §3.2 says is the
   actual predictive signal. Track `brefs_done` too: a sudden drop means the
   walker pruned a subtree behind a corrupt indirect block (§7.2).
4. **Scrub *before* you resilver, never only after a disk dies.** 8% of NetApp's
   mismatches were discovered during reconstruction — the one moment redundancy
   is gone. This is doubly true here, because per §7.4 `raid replace` performs no
   checksum verification of its own: any latent corruption on a surviving column
   is copied verbatim onto the new disk, and it will still be there after the
   array reports itself healthy. A clean scrub is what makes a replace safe.
5. **Watch dmesg — but do not trust it for volume.** CHECK-failure reporting goes
   through `krate_h2chk`, rate-limited to `.freq = 5`
   (`src/sys/local_hammer2_chain.c:89`), so a corruption storm is heavily
   under-reported in the log. Use the scrub counters for magnitude and dmesg for
   identification. `hammer2_characterize_failed_chain()`
   (`src/sys/local_hammer2_chain.c:5824`) walks up to the governing inode on a
   CHECK failure and prints the offending inode, PFS name, and device:
   ```
   chain <off>.<type> (<typename>) meth=<m> CHECK FAIL (flags=..., bref/data <expected>/<got>)
      Resides at/in inode <n>
      In pfs <name> on device <dev>
   ```
   `Resides in inode index - CRITICAL!!!` or `Resides in root index - CRITICAL!!!`
   mean the corruption is in metadata, which per §7.4 nothing will repair.
6. **Check array state before and after.** `hammer2 -s <mnt> raid status` reports
   per-disk `ONLINE` / `FAILED` plus resilver progress. Disks auto-failed by
   `hammer2_raid6_auto_fail_disk()` persist across remount, so a disk that
   silently dropped out during the week shows up here.
7. **Run `h2stripe_check` at the next planned unmount** to cover the parity-
   consistency class the online scrub cannot see (§7.5).
8. **Rehearse the failure.** The recovery paths are testable without waiting for
   real bitrot:
   - `sysctl vfs.hammer2.inject_eio_disk_mask=<bitmask>` forces EIO on chosen
     disk indices, exercising auto-fail and degraded reads
     (`tests/v3/test_e_eio_inject.sh`, `test_g_autofail.sh`).
   - `tests/v3/test_k_scrub.sh` K2 is a working bitrot drill: `dd` random bytes
     over one disk's stripe-data region while unmounted, remount, scrub, and
     confirm both that the pre-corruption sha256 is restored and that a follow-up
     scrub is clean.
   - `sysctl vfs.hammer2.resilver_skip_unalloc=0` forces a full-iteration
     resilver baseline for comparison against the default bitmap-aware path.
9. **Use ECC RAM, and keep XXHASH64.** Per §7.4, these two are where the
   remaining exposure actually is.

The regimen above is doing real work because of §7.4's first bullet. On ZFS,
ordinary reads quietly repair as they go and scrub is a background sweep for
what nobody has read lately. Here, scrub is the *only* thing that repairs
anything — an unscrubbed hammer2-raid6 array accumulates corruption exactly the
way §5.4 describes, with full redundancy sitting unused next to it.

---

## Sources

**CERN**
- [Bernd Panzer-Steindel, *Data integrity*, CERN/IT Draft 1.3, 8 April 2007](https://indico.cern.ch/event/13797/contributions/1362288/attachments/115080/163419/Data_integrity_v3.pdf)
- [Peter Kelemen, *Silent Corruptions*, LCSC 2007](https://www.nsc.liu.se/lcsc2007/presentations/LCSC_2007-kelemen.pdf)
- [StorageMojo, *CERN's data corruption research*](https://storagemojo.com/2007/09/19/cerns-data-corruption-research/)

**NetApp / Wisconsin**
- [Bairavasundaram et al., *An Analysis of Data Corruption in the Storage Stack*, FAST '08](https://www.usenix.org/legacy/event/fast08/tech/full_papers/bairavasundaram/bairavasundaram.pdf) ([NetApp page](https://www.netapp.com/atg/publications/publications-an-analysis-of-data-corruption-in-the-storage-stack-20081101/))
- [Bairavasundaram et al., *An Analysis of Latent Sector Errors in Disk Drives*, SIGMETRICS '07](https://research.cs.wisc.edu/wind/Publications/latent-sigmetrics07.pdf) ([ACM DL](https://dl.acm.org/doi/10.1145/1269899.1254917))
- [Maneas et al., *A Study of SSD Reliability in Large Scale Enterprise Storage Deployments*, FAST '20](https://www.usenix.org/system/files/fast20-maneas.pdf)
- [Zhang et al., *End-to-end Data Integrity for File Systems: A ZFS Case Study*, FAST '10](https://research.cs.wisc.edu/wind/Publications/zfs-corruption-fast10.pdf)

**Google**
- [Pinheiro, Weber, Barroso, *Failure Trends in a Large Disk Drive Population*, FAST '07](https://www.usenix.org/legacy/event/fast07/tech/full_papers/pinheiro/pinheiro.pdf)
- [Schroeder, Pinheiro, Weber, *DRAM Errors in the Wild*, SIGMETRICS '09](https://www.cs.toronto.edu/~bianca/papers/sigmetrics09.pdf)
- [Schroeder, Lagisetty, Merchant, *Flash Reliability in Production*, FAST '16](https://www.usenix.org/conference/fast16/technical-sessions/presentation/schroeder)
- [Hochschild et al., *Cores that don't count*, HotOS '21](https://sigops.org/s/conferences/hotos/2021/papers/hotos21-s01-hochschild.pdf)

**Meta / Facebook**
- [Dixit et al., *Silent Data Corruptions at Scale*, arXiv 2102.11245](https://arxiv.org/pdf/2102.11245)
- [Dixit et al., *Detecting silent data corruptions in the wild*, arXiv 2203.08989](https://ar5iv.labs.arxiv.org/html/2203.08989)
- [Meta Engineering, *Silent data corruption: mitigating effects at scale*](https://engineering.fb.com/2021/02/23/data-infrastructure/silent-data-corruption/)
- [Meza, Wu, Kumar, Mutlu, *Revisiting Memory Errors in Large-Scale Production Data Centers*, DSN '15](https://users.ece.cmu.edu/~omutlu/pub/memory-errors-at-facebook_dsn15.pdf)
- [Llama 3 training interruption breakdown (coverage)](https://www.datacenterdynamics.com/en/news/meta-report-details-hundreds-of-gpu-and-hbm3-related-interruptions-to-llama-3-training-run/)

**Alibaba**
- [Wang et al., *Understanding Silent Data Corruptions in a Large Production CPU Population*, SOSP '23](https://yangwang83.github.io/papers/sosp2023-Wang.pdf)
- [Han et al., *An In-Depth Study of Correlated Failures in Production SSD-Based Data Centers*, FAST '21](https://www.usenix.org/conference/fast21/presentation/han)
- [Xu et al., *General Feature Selection for Failure Prediction in Large-scale SSD Deployment*, DSN '21](https://www.cse.cuhk.edu.hk/~pclee/www/pubs/dsn21.pdf)
- [`alibaba-edu/dcbrain` SSD SMART logs](https://github.com/alibaba-edu/dcbrain/blob/master/ssd_smart_logs/readme.md)
- [Tianchi large-scale disk failure prediction dataset](https://tianchi.aliyun.com/dataset/70251)

**Microsoft**
- [Narayanan et al., *SSD Failures in Datacenters: What? When? and Why?*, SYSTOR '16](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/08/a7-narayanan.pdf)

**Public datasets**
- [Backblaze Hard Drive Test Data](https://www.backblaze.com/cloud-storage/resources/hard-drive-test-data) · [Drive Stats for 2025](https://www.backblaze.com/blog/backblaze-drive-stats-for-2025/) · [Q1 2026](https://www.backblaze.com/blog/backblaze-drive-stats-for-q1-2026/)
- [USENIX Computer Failure Data Repository (CFDR)](https://www.usenix.org/cfdr-data)
- [SMART-Z: a SMART dataset of 147,496 hard disks, *Scientific Data* (2025)](https://pmc.ncbi.nlm.nih.gov/articles/PMC12222913/) — data at [OSF, doi:10.17605/OSF.IO/24Y6G](https://doi.org/10.17605/OSF.IO/24Y6G)

*Note on access: every paper cited here is linked to a legitimate open-access copy —
USENIX proceedings, arXiv, PMC, or an author's/institution's own page. Nothing in this
literature required a paywall workaround.*

**AI-era SDC / industry**
- [OCP, *Computing's Hidden Menace: The OCP Takes Action Against Silent Data Corruption*](https://www.opencompute.org/blog/computings-hidden-menace-the-ocp-takes-action-against-silent-data-corruption-sdc)
- [OCP, *Silent Data Corruption in AI* whitepaper (NVIDIA et al.)](https://www.opencompute.org/documents/sdc-in-ai-ocp-whitepaper-final-pdf)
- [*Silent Data Corruption: Optimal Mitigation Strategies for Data Center Computing*, IEEE Micro, Jan 2026](https://www.computer.org/csdl/magazine/mi/2026/01/11301038/2cthVm7hbwI)
- [Ma, Pei, Lausen, Karypis, *Understanding Silent Data Corruption in LLM Training*, ACL 2025 / arXiv 2502.12340](https://arxiv.org/pdf/2502.12340)
- [*Exploring Silent Data Corruption as a Reliability Challenge in LLM Training* (TU Berlin, 2026)](https://arxiv.org/pdf/2604.00726)
- [*The Anatomy of Silent Data Corruption: GPU Error Pattern Study and Modeling Guidance* (2026)](https://arxiv.org/pdf/2605.04213)

**Other**
- [Amazon, *Beyond eleven nines: lessons from the Amazon S3 culture of durability*, re:Invent 2019](https://d1.awsstatic.com/events/reinvent/2019/REPEAT_1_Beyond_eleven_nines_Lessons_from_the_Amazon_S3_culture_of_durability_STG331-R1.pdf)
- [Rosenthal, *Keeping Bits Safe: How Hard Can It Be?*, ACM Queue](https://queue.acm.org/detail.cfm?id=1866298)
- [Rosenthal, *Empirical Measurements of Disk Failure Rates and Error Rates*, arXiv cs/0701166](https://arxiv.org/pdf/cs/0701166)
