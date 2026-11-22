#!/bin/sh
# Pi runs 2026-11-22: results in docs/encryption.md §6.2 (NEON + Adiantum: §6.6). Overwrites the first 4 GB of da0s1..da3s1.
# HAMMER2 RAID6 over 4 dm mappings (4 GB of each SSD slice): plain (linear) vs dm-crypt.
SECT=8388608; MB=1024; M=/mnt/cb; T=/mnt/cbsrc
mkdir -p $M $T
mount | grep -q " $T " || mount_tmpfs -o size=600m tmpfs $T
[ -s $T/src ] || dd if=/dev/urandom of=$T/src bs=1m count=512 2>/dev/null
echo "kern.dm_crypt.mp=$(sysctl -n kern.dm_crypt.mp 2>/dev/null || echo n/a) throttled=$(sysctl -n dev.bcmmbox.0.throttled 2>/dev/null)"
mb() { awk '/bytes transferred/{gsub(/\(/,"",$(NF-1)); printf "%d\n", $(NF-1)/1048576}'; }
now() { date +%s.%N 2>/dev/null | grep -q N && date +%s || date +%s.%N; }
run() { # label target-type key [neon_disable] [dm-crypt options, e.g. "1 sector_size:4096"]
  [ -n "$4" ] && sysctl -q hw.neon_crypto_disable=$4
  for i in 0 1 2 3; do
    if [ "$2" = linear ]; then tbl="0 $SECT linear /dev/da${i}s1 0"
    else tbl="0 $SECT crypt $2 $3 0 /dev/da${i}s1 0 $5"; fi
    dmsetup create b$i --table "$tbl" || return 1
  done
  D="/dev/mapper/b0 /dev/mapper/b1 /dev/mapper/b2 /dev/mapper/b3"
  newfs_hammer2 -R 6 -L BENCH $D > /dev/null 2>&1 || { echo "$1 newfs failed"; return 1; }
  S=/dev/mapper/b0:/dev/mapper/b1:/dev/mapper/b2:/dev/mapper/b3@BENCH
  mount -t hammer2 $S $M || return 1
  t0=$(date +%s); vmstat 1 >/tmp/vm.$1 & VP=$!
  dd if=$T/src of=$M/f1 bs=1m 2>/dev/null; dd if=$T/src of=$M/f2 bs=1m 2>/dev/null; sync; sync
  t1=$(date +%s); kill $VP
  w=$(( MB / (t1 - t0) ))
  umount $M; mount -t hammer2 $S $M
  r1=$(dd if=$M/f1 of=/dev/null bs=1m 2>&1 | mb)
  r2=$(dd if=$M/f2 of=/dev/null bs=1m 2>&1 | mb)
  idle=$(awk 'NR>3{s+=$NF;n++}END{if(n)printf "%d", s/n}' /tmp/vm.$1)
  echo "$1: write ${w} MB/s (1 GB + sync, $((t1-t0)) s, avg idle ${idle}%)  cold read ${r1} / ${r2} MB/s"
  umount $M
  for i in 0 1 2 3; do dmsetup remove b$i; done
  [ -n "$4" ] && sysctl -q hw.neon_crypto_disable=0
}
K64=$(openssl rand -hex 64); K32=$(openssl rand -hex 32)
for pass in 1 2; do
  run plain linear
  run xts-aes256 aes-xts-plain64 $K64
  sysctl -n hw.neon_crypto_disable >/dev/null 2>&1 && run xts-aes256-c aes-xts-plain64 $K64 1
  run xts-aes128 aes-xts-plain64 $K32
  run adiantum xchacha12,aes-adiantum-plain64 $K32
  run adiantum-4k xchacha12,aes-adiantum-plain64 $K32 "" "1 sector_size:4096"
done
umount $T
echo "throttled at end=$(sysctl -n dev.bcmmbox.0.throttled 2>/dev/null)"
echo DONE
