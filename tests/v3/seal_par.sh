#!/bin/sh
# Informational: seal timing (oct-review P3 step 3) with parallel writers.
# N writers each dd PAR_MB of incompressible data, then sync.
SCRIPTDIR=$(dirname "$0")
. "$SCRIPTDIR/common.sh"
kldstat -q -m hammer2 || kldload hammer2
N=${PAR_N:-4}
MB=${PAR_MB:-64}
SRC=/var/tmp/p_rand256
[ -s $SRC ] || dd if=/dev/urandom of=$SRC bs=1048576 count=256 2>/dev/null
setup_fresh full
sync; sleep 2
w0=$(sysctl -n vfs.hammer2.rz_pq_wait_ns); t0=$(sysctl -n vfs.hammer2.rz_seal_ns)
n0=$(sysctl -n vfs.hammer2.rz_seal_cnt)
/usr/bin/time -p sh -c "
i=0
while [ \$i -lt $N ]; do
    dd if=$SRC of=$MNTPT/w\$i bs=1048576 count=$MB skip=\$((i * 8)) 2>/dev/null &
    i=\$((i + 1))
done
wait
sync; sync" 2> /var/tmp/seal_par_time.txt
secs=$(awk '$1 == "real" { print $2 }' /var/tmp/seal_par_time.txt)
w=$(( $(sysctl -n vfs.hammer2.rz_pq_wait_ns) - w0 ))
t=$(( $(sysctl -n vfs.hammer2.rz_seal_ns) - t0 ))
n=$(( $(sysctl -n vfs.hammer2.rz_seal_cnt) - n0 ))
echo "  PAR x$N ${MB}MB: $n rows, P/Q wait $((w / 1000000)) ms, in seal $((t / 1000000)) ms, wall $secs s," \
     "$(awk -v m=$((N * MB)) -v s=$secs 'BEGIN { printf "%.1f", m / s }') MB/s"
teardown PAR
