#!/bin/sh
# report.sh — parse fio JSON results into a markdown summary.
# Usage: sh report.sh <results_dir>   > summary.md
#
# Needs: awk + a small inline python helper (python3 in base on DragonFly).

DIR="${1:-.}"

python3 - "$DIR" <<'PY'
import json, os, sys, glob
from collections import defaultdict

root = sys.argv[1]
files = sorted(glob.glob(os.path.join(root, "*.json")))
if not files:
    print(f"# perf summary\n\nNo fio JSON in {root}")
    sys.exit(0)

# rows[(workload, jobs)][cfg] = (bw_mib, iops, p99_us)
rows = defaultdict(dict)
configs_seen = []

for f in files:
    name = os.path.basename(f).rsplit(".json", 1)[0]
    try:
        cfg, wl, jtag = name.rsplit("_", 2)
    except ValueError:
        continue
    if not jtag.startswith("j"):
        continue
    jobs = int(jtag[1:])
    try:
        with open(f) as fh:
            data = json.load(fh)
    except Exception as e:
        rows[(wl, jobs)][cfg] = ("ERR", "ERR", "ERR")
        continue
    j = data["jobs"][0]
    # pick whichever side dominates the workload
    rd, wr = j["read"], j["write"]
    bw_kib = rd["bw"] + wr["bw"]
    iops = rd["iops"] + wr["iops"]
    # p99 in ns -> us
    def p99(side):
        clat = side.get("clat_ns") or side.get("clat")
        if not clat: return None
        pct = clat.get("percentile", {})
        v = pct.get("99.000000") or pct.get("99.0")
        if v is None: return None
        return v / 1000.0 if "clat_ns" in side else v
    p = p99(rd) or p99(wr)
    rows[(wl, jobs)][cfg] = (bw_kib / 1024.0, iops, p)
    if cfg not in configs_seen:
        configs_seen.append(cfg)

# stable cfg order
preferred = ["h2-1disk", "v3-healthy", "v3-degraded"]
configs = [c for c in preferred if c in configs_seen] + \
          [c for c in configs_seen if c not in preferred]

print("# perf summary")
print(f"\nResults dir: `{root}`")
print(f"\nConfigs: {', '.join(configs)}\n")

for (wl, jobs) in sorted(rows.keys()):
    print(f"## {wl}, numjobs={jobs}\n")
    print("| config | BW (MiB/s) | IOPS | p99 lat (us) | vs h2-1disk |")
    print("|---|---:|---:|---:|---:|")
    base = rows[(wl, jobs)].get("h2-1disk")
    base_bw = base[0] if base and isinstance(base[0], float) else None
    for cfg in configs:
        r = rows[(wl, jobs)].get(cfg)
        if not r:
            print(f"| {cfg} | - | - | - | - |")
            continue
        bw, iops, p99v = r
        if isinstance(bw, float):
            bw_s = f"{bw:.1f}"
            iops_s = f"{iops:.0f}"
            p99_s = f"{p99v:.0f}" if p99v else "-"
            if base_bw and cfg != "h2-1disk":
                ratio = (bw / base_bw - 1.0) * 100.0
                ratio_s = f"{ratio:+.0f}%"
            else:
                ratio_s = "—"
        else:
            bw_s = iops_s = p99_s = ratio_s = "ERR"
        print(f"| {cfg} | {bw_s} | {iops_s} | {p99_s} | {ratio_s} |")
    print()
PY
