# Archive

Design / planning docs from the pre-rewrite era (RAID6 as a layer
*below* HAMMER2, with UFS-backed vn-device test substrate).  This
implementation was deleted in Phase 1 — see
`docs/phase1_changelog.md` — and replaced with the v3 RAIDZ2-native
design currently shipping (volume version
`HAMMER2_VOL_VERSION_RAIDZ2 = 3`).

Kept here for historical context.  The current code is documented
in:

- `docs/DEVELOPER.md` — reference for the v3 implementation
- `docs/newplan.md` — master plan
- `docs/zfs_compare.md` — ZFS comparison + ROI-ordered improvements
- `docs/phase1_changelog.md` — what Phase 1 deleted and why
- `docs/outstanding.md` — open items
- `docs/{stripe_bitmap,metadata_zone,resilver_v3,volhdr_quorum}.md`
  — v3 on-disk format / I/O specs
- `docs/raidz2native_test_spec.md` — test suite spec

## Contents

| Doc | Era | What it was |
|---|---|---|
| `RAID6_MERGE_REQUEST.md` | pre-rewrite | Initial merge proposal for RAID6-below-HAMMER2 |
| `NEXT_STEPS.md` | pre-rewrite | Pre-rewrite work plan; superseded by `docs/newplan.md` |
| `tracker.md` | Phase 0 / Phase 1 | Deletion tracker for the pre-rewrite layer; closed |
| `inplace_audit.md` | Phase 0 | Audit of in-place overwrite sites that needed deletion |
| `write_hole.md` | pre-rewrite | Write-intent-bitmap analysis; not applicable to v3 RAIDZ2-native (COW + TXG-commit closes the write hole) |
| `raidz2_in_hammer2.md` | foundational | Design input for integrating RAIDZ2 ideas into HAMMER2's COW layer; folded into `docs/newplan.md` |
| `raidz2_snapshot_interaction.md` | foundational | Snapshot semantics under RAIDZ2; folded into `docs/newplan.md` |
| `zfs_freemap_design.md` | design study | Analysis of ZFS-like RAID freemap; not the path taken |
| `blockingreads.txt` | debug log | vn/UFS deadlock investigation from the early pre-rewrite era |
