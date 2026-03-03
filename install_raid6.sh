#!/bin/sh
#
# install_raid6.sh — Apply HAMMER2 RAID 6 patch and build/install
#
# Usage:
#   ./install_raid6.sh [patch|build|install|all]
#
# Requires: DragonFlyBSD with /usr/src populated (matching running kernel)
# Must be run as root.
#
# IMPORTANT: hammer2.ko cannot be kldunload'd while the root filesystem uses
# it.  After installing, a reboot is required for the new module to take effect.
# Use `cp hammer2.ko /boot/kernel/hammer2.ko` to stage the module before reboot.
#

set -e

PATCH_FILE="$(dirname "$0")/hammer2_raid6.patch"
SRCDIR="/usr/src"

usage() {
    echo "Usage: $0 [patch|build|install|all]"
    echo ""
    echo "  patch   — Apply the RAID 6 patch to /usr/src"
    echo "  build   — Build the hammer2 kernel module and userspace tools"
    echo "  install — Install the built artifacts"
    echo "  all     — Do all of the above (default)"
    exit 1
}

check_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "Error: must be run as root"
        exit 1
    fi
}

check_srcdir() {
    if [ ! -d "$SRCDIR/sys/vfs/hammer2" ]; then
        echo "Error: $SRCDIR/sys/vfs/hammer2 not found"
        echo "Please ensure /usr/src is populated with DragonFlyBSD sources"
        exit 1
    fi
}

do_patch() {
    echo "==> Applying HAMMER2 RAID 6 patch..."
    if [ ! -f "$PATCH_FILE" ]; then
        echo "Error: patch file not found: $PATCH_FILE"
        exit 1
    fi

    cd "$SRCDIR"

    # Dry-run first
    if ! patch --check -p1 < "$PATCH_FILE" > /dev/null 2>&1; then
        echo "Warning: patch dry-run failed. Attempting with -N (skip applied)..."
        if ! patch --check -N -p1 < "$PATCH_FILE" > /dev/null 2>&1; then
            echo "Error: patch cannot be applied cleanly"
            exit 1
        fi
        echo "Some hunks already applied, using -N"
        patch -N -p1 < "$PATCH_FILE"
    else
        patch -p1 < "$PATCH_FILE"
    fi

    echo "==> Patch applied successfully"
}

do_build() {
    echo "==> Building HAMMER2 kernel module..."
    cd "$SRCDIR/sys/vfs/hammer2"
    make clean > /dev/null 2>&1 || true
    make

    echo "==> Building newfs_hammer2..."
    cd "$SRCDIR/sbin/newfs_hammer2"
    make clean > /dev/null 2>&1 || true
    make

    echo "==> Building hammer2 utility..."
    cd "$SRCDIR/sbin/hammer2"
    make clean > /dev/null 2>&1 || true
    make

    echo "==> Build complete"
}

do_install() {
    echo "==> Installing HAMMER2 kernel module..."
    # Copy directly — make install may not handle kld path correctly on all setups
    cp "$SRCDIR/sys/vfs/hammer2/hammer2.ko" /boot/kernel/hammer2.ko
    echo "    hammer2.ko -> /boot/kernel/hammer2.ko"

    echo "==> Installing newfs_hammer2..."
    # Use install(1), not cp — cp fails with "Text file busy" on running binaries
    install -m 755 "$SRCDIR/sbin/newfs_hammer2/newfs_hammer2" /sbin/newfs_hammer2
    echo "    newfs_hammer2 -> /sbin/newfs_hammer2"

    echo "==> Installing hammer2 utility..."
    install -m 755 "$SRCDIR/sbin/hammer2/hammer2" /sbin/hammer2
    echo "    hammer2 -> /sbin/hammer2"

    echo ""
    echo "==> Installation complete"
    echo ""
    echo "NOTE: hammer2.ko has been staged to /boot/kernel/hammer2.ko."
    echo "      A reboot is required to activate the new kernel module"
    echo "      (the root filesystem uses hammer2, so kldunload is not possible)."
    echo ""
    echo "After reboot, format a RAID 6 filesystem (minimum 4 disks):"
    echo "  newfs_hammer2 -R 6 -L DATA /dev/da0 /dev/da1 /dev/da2 /dev/da3"
    echo ""
    echo "Mount it:"
    echo "  mount -t hammer2 /dev/da0:/dev/da1:/dev/da2:/dev/da3@DATA /mnt/data"
    echo ""
    echo "Check RAID status:"
    echo "  hammer2 raid status /dev/da0"
    echo ""
    echo "Mark a disk as failed:"
    echo "  hammer2 -s /mnt/data raid fail-disk /dev/da2"
    echo ""
    echo "Replace a failed disk (online resilver):"
    echo "  hammer2 -s /mnt/data raid replace /dev/da2 /dev/da4"
    echo ""
    echo "Create a snapshot:"
    echo "  hammer2 snapshot /mnt/data mysnap"
    echo ""
    echo "Mount a snapshot:"
    echo "  mount -t hammer2 /dev/da0:/dev/da1:/dev/da2:/dev/da3@mysnap /mnt/snap"
}

# Parse command
ACTION="${1:-all}"

check_root
check_srcdir

case "$ACTION" in
    patch)
        do_patch
        ;;
    build)
        do_build
        ;;
    install)
        do_install
        ;;
    all)
        do_patch
        do_build
        do_install
        ;;
    *)
        usage
        ;;
esac
