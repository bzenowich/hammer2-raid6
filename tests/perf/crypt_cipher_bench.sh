#!/bin/sh
# dm-crypt cipher cost alone: crypt mappings over a dm zero target (docs/encryption.md §6.1).
# Needs dm_target_crypt loaded. Touches no disk.  On arm64 the XTS and Adiantum
# rows run twice, NEON and portable C (hw.neon_crypto_disable).
K64=$(openssl rand -hex 64); K32=$(openssl rand -hex 32); K16=$(openssl rand -hex 16)
SECT=8388608
echo "kern.dm_crypt.mp=$(sysctl -n kern.dm_crypt.mp 2>/dev/null || echo n/a) throttled=$(sysctl -n dev.bcmmbox.0.throttled 2>/dev/null)"
mb() { awk '/bytes transferred/{gsub(/\(/,"",$(NF-1)); printf "%d\n", $(NF-1)/1048576}'; }
t() {
  w=$(dd if=/dev/zero of=$1 bs=1m count=1024 2>&1 | mb)
  r=$(dd if=$1 of=/dev/null bs=1m count=1024 2>&1 | mb)
  echo "$2 write $w MB/s  read $r MB/s"
}
dmsetup create z0 --table "0 $SECT zero" || exit 1
t /dev/mapper/z0 "zero-baseline       "
# label spec key neon(0/1/-): new sessions pick NEON or C by hw.neon_crypto_disable
NEONOK=$(sysctl -n hw.neon_crypto_disable 2>/dev/null)
for spec in "xts-aes256-neon aes-xts-plain64 $K64 1" "xts-aes256-c aes-xts-plain64 $K64 0" \
    "xts-aes128-neon aes-xts-plain64 $K32 1" "xts-aes128-c aes-xts-plain64 $K32 0" \
    "adiantum-neon xchacha12,aes-adiantum-plain64 $K32 1" "adiantum-c xchacha12,aes-adiantum-plain64 $K32 0" \
    "cbc-aes256-essiv aes-cbc-essiv:sha256 $K32 -" "cbc-aes128 aes-cbc-plain64 $K16 -" \
    "xts-twofish256 twofish-xts-plain64 $K64 -" "xts-serpent256 serpent-xts-plain64 $K64 -"; do
  set -- $spec
  [ "$4" != - ] && [ -z "$NEONOK" ] && [ "$4" = 1 ] && continue   # no NEON ciphers here
  [ "$4" != - ] && [ -n "$NEONOK" ] && sysctl -q hw.neon_crypto_disable=$((1 - $4))
  if dmsetup create c0 --table "0 $SECT crypt $2 $3 0 /dev/mapper/z0 0" 2>/dev/null; then
    t /dev/mapper/c0 "$(printf '%-20s' $1)"; dmsetup remove c0
  else echo "$1: table refused"; fi
  [ -n "$NEONOK" ] && sysctl -q hw.neon_crypto_disable=0
done
# 4 parallel streams on 4 separate mappings
par() { # label spec key
  for i in 0 1 2 3; do dmsetup create p$i --table "0 $SECT crypt $2 $3 0 /dev/mapper/z0 0"; done
  for i in 0 1 2 3; do dd if=/dev/zero of=/dev/mapper/p$i bs=1m count=512 2>&1 | mb > /tmp/pw$i & done; wait
  echo "4x parallel $1 write MB/s each: $(cat /tmp/pw0 /tmp/pw1 /tmp/pw2 /tmp/pw3 | tr '\n' ' ')"
  for i in 0 1 2 3; do dd if=/dev/mapper/p$i of=/dev/null bs=1m count=512 2>&1 | mb > /tmp/pr$i & done; wait
  echo "4x parallel $1 read  MB/s each: $(cat /tmp/pr0 /tmp/pr1 /tmp/pr2 /tmp/pr3 | tr '\n' ' ')"
  for i in 0 1 2 3; do dmsetup remove p$i; done
}
par xts-aes256 aes-xts-plain64 $K64
[ -n "$NEONOK" ] && par adiantum xchacha12,aes-adiantum-plain64 $K32
dmsetup remove z0
echo "throttled at end=$(sysctl -n dev.bcmmbox.0.throttled 2>/dev/null)"
