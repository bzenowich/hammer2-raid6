#!/bin/sh
# run_perf.sh — drive fio across {h2-1disk, v3-healthy, v3-degraded}
# x {seq-read-1m, seq-write-1m, rand-read-4k, pg-mix} x numjobs.
# Emits results/<cfg>_<workload>_j<N>.json plus a summary.md.
#
# Usage: sh run_perf.sh [config ...] [-- workload ...]
#   Default configs:    h2-1disk v3-healthy v3-degraded
#   Default workloads:  seq-read-1m seq-write-1m rand-read-4k pg-mix
#   Override threads:   JOBS="1 4 8 16"  sh run_perf.sh
#   Override NDISKS:    NDISKS=6 sh run_perf.sh
#
# Run as root inside the harness VM.

SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"
require_fio

JOBS="${JOBS:-1 4 8 16}"
RUNTAG="${RUNTAG:-$(date +%Y%m%d-%H%M%S)}"
RESULTS="${RESULTS:-${SCRIPTDIR}/results/${RUNTAG}}"
mkdir -p "$RESULTS"
echo "==> Results dir: $RESULTS"

ALL_CFGS="h2-1disk v3-healthy v3-degraded"
ALL_WLS="seq-read-1m seq-write-1m rand-read-4k pg-mix"

# Parse args:  configs [-- workloads]
CFGS=""
WLS=""
in_wls=0
for a in "$@"; do
    if [ "$a" = "--" ]; then in_wls=1; continue; fi
    if [ "$in_wls" -eq 0 ]; then CFGS="$CFGS $a"; else WLS="$WLS $a"; fi
done
CFGS="${CFGS:-$ALL_CFGS}"
WLS="${WLS:-$ALL_WLS}"

kldstat -q -m hammer2 || kldload hammer2

run_workload() {
    local cfg="$1"
    local wl="$2"
    local jobs="$3"
    local job_file="$SCRIPTDIR/jobs/${wl}.fio"
    local out="$RESULTS/${cfg}_${wl}_j${jobs}.json"
    [ -r "$job_file" ] || { echo "SKIP: no $job_file"; return; }

    # Drop the FS cache via remount for a cold-ish read run.
    case "$wl" in
        *-read*) remount_cycle "$ACTIVE_PFS" || true ;;
    esac

    snapshot_sysctl "${cfg}_${wl}_j${jobs}_pre"
    local cf_pre
    cf_pre=$(dmesg_checkfails)

    fio --output-format=json+ --output="$out" \
        --directory="$MNTPT" --numjobs="$jobs" "$job_file" \
        >/dev/null 2>&1
    local rc=$?

    snapshot_sysctl "${cfg}_${wl}_j${jobs}_post"
    local cf_post
    cf_post=$(dmesg_checkfails)
    if [ "$cf_post" != "$cf_pre" ]; then
        echo "  WARN  $cfg/$wl/j$jobs: CHECK FAIL delta $cf_pre -> $cf_post"
    fi

    if [ $rc -ne 0 ]; then
        echo "  FAIL  $cfg/$wl/j$jobs (fio rc=$rc)"
    else
        echo "  ok    $cfg/$wl/j$jobs -> $(basename "$out")"
    fi
}

for cfg in $CFGS; do
    echo "########## config: $cfg ##########"
    case "$cfg" in
        h2-1disk)     setup_h2_1disk;    ACTIVE_PFS="$PFSPATH_1D" ;;
        v3-healthy)   setup_v3_healthy;  ACTIVE_PFS="$PFSPATH_V3" ;;
        v3-degraded)  setup_v3_degraded; ACTIVE_PFS="$PFSPATH_V3" ;;
        *) echo "  unknown config: $cfg"; continue ;;
    esac

    for wl in $WLS; do
        for j in $JOBS; do
            run_workload "$cfg" "$wl" "$j"
        done
    done

    teardown_config
done

echo "==> Generating summary"
sh "$SCRIPTDIR/report.sh" "$RESULTS" > "$RESULTS/summary.md"
echo "==> summary: $RESULTS/summary.md"
