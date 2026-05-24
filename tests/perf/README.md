# tests/perf — HAMMER2 v3 RAIDZ2-native performance harness

Drives [fio](https://github.com/axboe/fio) across three configurations on the
DragonFly harness VM:

| config        | layout                                       |
|---------------|----------------------------------------------|
| `h2-1disk`    | vanilla HAMMER2 on `/dev/vbd1` (baseline)    |
| `v3-healthy`  | v3 RAIDZ2-native across `/dev/vbd1..vbdN`            |
| `v3-degraded` | v3 RAIDZ2-native with `/dev/vbd3` failed at runtime  |

`vbd0` is the system disk on the harness VM (see `tests/v3/common.sh`
header note); test disks start at `vbd1`.  `DISK_BASE=0` overrides the
offset if your substrate places the FS root elsewhere.

Workloads (`jobs/*.fio`):

- `seq-read-1m`  — 1 MiB sequential read, runtime 60s
- `seq-write-1m` — 1 MiB sequential write, runtime 60s
- `rand-read-4k` — 4 KiB random read, iodepth 32
- `pg-mix`       — 8 KiB 70/30 randrw, fsync every 8 ops (Postgres-ish)

Each workload runs at `numjobs=1,4,8,16` (override with `JOBS=`).

## Install fio on the VM

fio is not in the DragonFly base pkg repo.  Build from source:

    fetch -o /tmp/fio.tgz https://github.com/axboe/fio/archive/fio-3.36.tar.gz
    cd /tmp && tar xzf fio.tgz && cd fio-fio-3.36
    ./configure && make && make install

(Or skip fio and use only the report.sh JSON ingestion later — not yet wired.)

## Run

From the host:

    ./deploy.sh tests                              # syncs tests/ to VM
    ssh h2dev 'cd /root/hammer2-tests/perf && sh run_perf.sh'

Override:

    ssh h2dev 'cd /root/hammer2-tests/perf && \
        NDISKS=6 JOBS="1 8" sh run_perf.sh v3-healthy -- seq-read-1m'

Output:

- `tests/perf/results/<timestamp>/<cfg>_<workload>_jN.json` — raw fio JSON
- `tests/perf/results/<timestamp>/sysctl_*.txt` — pre/post `vfs.hammer2.*`
- `tests/perf/results/<timestamp>/summary.md` — markdown table

## Acceptance targets (suggested)

Numbers to validate after the first run lands a baseline:

- `v3-healthy seq-read`  >= `h2-1disk * (ndata * 0.75)` (4-disk -> 1.5x)
- `v3-healthy seq-write` >= `h2-1disk * 0.6` (P+Q overhead)
- `v3-healthy rand-read` >= `h2-1disk * 0.9`
- `v3-degraded` produces **zero** `CHECK FAIL` in dmesg
- `pg-mix` regression < 25% vs `h2-1disk`

The harness records dmesg `CHECK FAIL` count pre/post each run and prints a
WARN if it grew — useful for catching parity-path regressions hiding under
"the throughput looks fine."

## Caveats

- VM disks are qcow2 on the host SSD — absolute numbers are not portable;
  only intra-run ratios are meaningful.  For real numbers, re-run on the
  physical hardware target (see `memory/hardware_target.md`).
- DragonFly does not have an `O_DIRECT` equivalent at the FS layer; `pg-mix`
  uses `fsync=8` to approximate commit pressure.
- `setup_v3_degraded` fails disk index 2; for stripe 0 with NDISKS=4 that is
  a data column.  For NDISKS=5 or 6, index 2 is still a data column at
  stripe 0.  Adjust if testing higher disk counts.
