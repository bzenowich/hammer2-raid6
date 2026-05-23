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
#   DFLY_HOST — VM SSH host alias (default: h2dev)

DFLY_HOST="${DFLY_HOST:-h2dev}"
VM="${DFLY_HOST}"
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
        scp_as "$f" "/usr/src/sys/vfs/hammer2/$name"
    done

    echo "==> Syncing additional VFS sources -> /usr/src/sys/vfs/hammer2/"
    scp_as "$DIR/src/sys/local_hammer2_strategy.c" \
        "/usr/src/sys/vfs/hammer2/hammer2_strategy.c"
    scp_as "$DIR/src/sys/local_hammer2_freemap.c" \
        "/usr/src/sys/vfs/hammer2/hammer2_freemap.c"

    echo "==> Syncing kernel Makefile -> /usr/src/sys/vfs/hammer2/Makefile"
    scp_as "$DIR/src/sys/local_Makefile" \
        "/usr/src/sys/vfs/hammer2/Makefile"

    echo "==> Patching /usr/src/sys/conf/files for hammer2_raid6.c"
    ssh "$VM" sh <<'ENDSSH'
set -e
F=/usr/src/sys/conf/files
if grep -q '^vfs/hammer2/hammer2_raid6\.c' "$F"; then
    echo "    already present"
else
    sed -i '' '/^vfs\/hammer2\/hammer2_ondisk\.c.*optional hammer2$/a\
vfs/hammer2/hammer2_raid6.c	optional hammer2
' "$F"
    echo "    inserted hammer2_raid6.c entry"
fi
ENDSSH

    echo "==> Syncing newfs_hammer2 sources -> /usr/src/sbin/newfs_hammer2/"
    for f in "$DIR/src/sbin"/local_mkfs_*.c \
             "$DIR/src/sbin"/local_mkfs_hammer2.h \
             "$DIR/src/sbin"/local_newfs_hammer2.c; do
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
    echo "==> Building on VM (${DFLY_HOST})..."
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
# Pick the right hammer2.ko.  The KMOD-style /usr/src/sys/vfs/hammer2/
# hammer2.ko (produced by `make` inside the VFS dir) is a valid ELF
# relocatable but the DragonFly loader rejects it with "file has no
# contents" — it lacks the linker_set metadata the kernel-tree build
# emits.  Prefer the kernel-tree-built .ko (lives under /usr/obj for
# whichever KERNCONF was last built); fall back to the KMOD .ko with
# a warning.
KO_KMOD=/usr/src/sys/vfs/hammer2/hammer2.ko
KO_OBJ=""
for cfg in H2DEV X86_64_GENERIC; do
    candidate=/usr/obj/usr/src/sys/${cfg}/usr/src/sys/vfs/hammer2/hammer2.ko
    if [ -f "$candidate" ]; then
        KO_OBJ="$candidate"
        break
    fi
done
if [ -n "$KO_OBJ" ]; then
    cp "$KO_OBJ" /boot/kernel/hammer2.ko
    echo "    installed kernel-tree hammer2.ko ($(stat -f %z "$KO_OBJ") bytes)"
    echo "    from $KO_OBJ"
else
    echo "    WARNING: no kernel-tree hammer2.ko found under /usr/obj."
    echo "             The KMOD .ko at $KO_KMOD is NOT loader-compatible."
    echo "             Run 'cd /usr/src && make nativekernel KERNCONF=H2DEV"
    echo "             && make reinstallkernel KERNCONF=H2DEV' first."
    exit 1
fi
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
