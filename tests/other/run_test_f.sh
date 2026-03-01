#!/bin/sh
BEFORE=$(dmesg | wc -l)
sh /var/tmp/test_f_sequential_fail.sh > /tmp/test_f.log 2>&1
TEXIT=$?
echo "=== TEST OUTPUT ==="
cat /tmp/test_f.log
echo ""
echo "=== NEW DMESG ==="
dmesg | tail -n +$BEFORE
echo "=== EXIT: $TEXIT ==="
