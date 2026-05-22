#!/bin/sh
# apply.sh — run inside the guest as root, after the installer finishes,
# to bake the harness configuration into the base image.
#
# Usage:
#   1. Boot the freshly installed guest from disk (serial via host socat).
#   2. After first login, mount the host-pushed copy of this directory
#      (or paste each file by hand if the network is not yet up).
#   3. sh apply.sh
#   4. shutdown -p now
#   5. Promote the image on the host: see docs/harness.md.

set -eu

cd "$(dirname "$0")"

echo "== /boot/loader.conf"
# Remove any prior FreeBSD-style comma-list console value before appending.
if [ -f /boot/loader.conf ] && grep -q '^console=.*comconsole,vidconsole' /boot/loader.conf; then
    sed -i '' '/^console=.*comconsole,vidconsole/d' /boot/loader.conf
fi
if ! grep -q '^comconsole_speed=' /boot/loader.conf 2>/dev/null; then
    cat loader.conf >> /boot/loader.conf
fi

echo "== /etc/ttys"
# Replace whichever ttyu0 line is present (commented or not).
if grep -q '^ttyu0' /etc/ttys; then
    sed -i '' '/^ttyu0/d' /etc/ttys
elif grep -q '^#ttyu0' /etc/ttys; then
    sed -i '' '/^#ttyu0/d' /etc/ttys
fi
cat ttys.fragment >> /etc/ttys

echo "== /etc/rc.conf"
if ! grep -q '^hostname="h2dev"' /etc/rc.conf 2>/dev/null; then
    cat rc.conf.fragment >> /etc/rc.conf
fi

echo "== /etc/sysctl.conf"
if ! grep -q '^debug.debugger_on_panic=' /etc/sysctl.conf 2>/dev/null; then
    cat sysctl.conf.fragment >> /etc/sysctl.conf
fi

echo "== /root/.ssh/authorized_keys"
mkdir -p /root/.ssh
chmod 700 /root/.ssh
if ! grep -q 'h2dev-harness' /root/.ssh/authorized_keys 2>/dev/null; then
    cat authorized_keys >> /root/.ssh/authorized_keys
fi
chmod 600 /root/.ssh/authorized_keys

echo "== ensure rsync is installed"
if ! command -v rsync >/dev/null 2>&1; then
    echo "rsync not present; installing via pkg"
    pkg install -y rsync || echo "WARN: pkg install rsync failed; install manually before push/pull works"
fi

echo "== done. shutdown -p now, then promote the disk image on the host."
