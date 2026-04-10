#!/bin/bash
# deploy.sh — Sync modified sources to the DragonFlyBSD VM and build.
#
# Usage: ./deploy.sh [sync|build|install|tests|all]
#   sync    — Copy modified sources to the VM (strips local_ prefix)
#   build   — Build hammer2.ko + newfs_hammer2 + hammer2 on the VM
#   install — Install built artifacts on the VM
#   tests   — Sync test scripts to /root/hammer2-tests/ on the VM
#   all     — sync + build + install + tests (default)
#
# Environment:
#   DFLY_IP — VM IP address (default: 192.168.25.66)

DFLY_IP="${DFLY_IP:-192.168.25.66}"
VM="root@${DFLY_IP}"
DIR="$(cd "$(dirname "$0")" && pwd)"

# scp a local file to the VM, renaming it to $dst
scp_as() {
    local src="$1"
    local dst="$2"
    printf "  %-45s -> %s\n" "$(basename "$src")" "$dst"
    scp -q "$src" "${VM}:${dst}"
}

do_sync() {
    echo "==> Syncing kernel VFS sources -> /usr/src/sys/vfs/hammer2/"
    for f in "$DIR/src/sys"/local_*.c "$DIR/src/sys"/local_*.h; do
        [ -f "$f" ] || continue
        base="$(basename "$f")"
        name="${base#local_}"
        if [ "$base" = "local_vn.c" ]; then
            scp_as "$f" "/usr/src/sys/dev/disk/vn/vn.c"
        else
            scp_as "$f" "/usr/src/sys/vfs/hammer2/$name"
        fi
    done

    echo "==> Syncing additional VFS sources -> /usr/src/sys/vfs/hammer2/"
    scp_as "$DIR/src/sys/local_hammer2_strategy.c" \
        "/usr/src/sys/vfs/hammer2/hammer2_strategy.c"
    scp_as "$DIR/src/sys/local_hammer2_freemap.c" \
        "/usr/src/sys/vfs/hammer2/hammer2_freemap.c"

    echo "==> Syncing newfs_hammer2 sources -> /usr/src/sbin/newfs_hammer2/"
    for f in "$DIR/src/sbin"/local_mkfs_*.c; do
        [ -f "$f" ] || continue
        base="$(basename "$f")"
        scp_as "$f" "/usr/src/sbin/newfs_hammer2/${base#local_}"
    done

    echo "==> Syncing hammer2 utility sources -> /usr/src/sbin/hammer2/"
    for f in "$DIR/src/sbin"/local_cmd_*.c \
             "$DIR/src/sbin"/local_hammer2_userspace.h; do
        [ -f "$f" ] || continue
        base="$(basename "$f")"
        scp_as "$f" "/usr/src/sbin/hammer2/${base#local_}"
    done
}

do_build() {
    echo "==> Building on VM (${DFLY_IP})..."
    ssh "$VM" sh <<'ENDSSH'
set -e
echo "--- hammer2 kernel module ---"
cd /usr/src/sys/vfs/hammer2
make clean > /dev/null 2>&1 || true
make

echo "--- newfs_hammer2 ---"
cd /usr/src/sbin/newfs_hammer2
make clean > /dev/null 2>&1 || true
make

echo "--- hammer2 utility ---"
cd /usr/src/sbin/hammer2
make clean > /dev/null 2>&1 || true
make

echo "--- Build complete ---"
ENDSSH
}

do_install() {
    echo "==> Installing on VM..."
    ssh "$VM" sh <<'ENDSSH'
set -e
cp /usr/src/sys/vfs/hammer2/hammer2.ko /boot/kernel/hammer2.ko
install -m 755 /usr/src/sbin/newfs_hammer2/newfs_hammer2 /sbin/newfs_hammer2
install -m 755 /usr/src/sbin/hammer2/hammer2 /sbin/hammer2
echo "--- Install complete ---"
echo "    NOTE: reboot required to activate the new hammer2.ko"
ENDSSH
}

do_tests() {
    echo "==> Syncing test scripts -> /root/hammer2-tests/"
    ssh "$VM" mkdir -p /root/hammer2-tests
    # Use tar to preserve directory structure (rsync may not be on DragonFlyBSD)
    tar -C "$DIR" -cf - tests | ssh "$VM" tar -xf - -C /root/hammer2-tests --strip-components=1
    echo "    Tests synced to /root/hammer2-tests/"

    echo "==> Syncing diagnostic tools -> /root/h2diag/"
    ssh "$VM" mkdir -p /root/h2diag
    tar -C "$DIR/src" -cf - diag | ssh "$VM" tar -xf - -C /root/h2diag --strip-components=1
    echo "    Diagnostics synced to /root/h2diag/"
}

ACTION="${1:-all}"

case "$ACTION" in
    sync)    do_sync ;;
    build)   do_build ;;
    install) do_install ;;
    tests)   do_tests ;;
    all)
        do_sync
        do_build
        do_install
        do_tests
        ;;
    *)
        echo "Usage: $0 [sync|build|install|tests|all]"
        exit 1
        ;;
esac
