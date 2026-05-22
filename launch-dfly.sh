#!/bin/bash
# launch-dfly.sh — start the harness VM.
#
# Modes (first positional arg):
#   run      (default) boot from images/overlay-system.qcow2
#   install  boot from CDROM (iso/dfly-*_REL.iso) to install DragonFly
#   reset    delete the overlay and recreate it from the locked base image
#
# Environment overrides:
#   NDISKS=4                number of RAID6 test disks (default 4, max ~10)
#   RAM=4G                  guest memory
#   CPUS=4                  vCPUs
#   FOREGROUND=1            do not daemonize (block until QEMU exits)
#   ISO=path/to.iso         override CDROM image for install mode
#   SSH_PORT=2322           host forwarding port for guest SSH

set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

mkdir -p run logs images iso

MODE="${1:-run}"
NDISKS="${NDISKS:-4}"
RAM="${RAM:-4G}"
CPUS="${CPUS:-4}"
SSH_PORT="${SSH_PORT:-2322}"

BASE="images/base-dfly-6.4.2.qcow2"
SYS="images/overlay-system.qcow2"
SYS_SIZE="${SYS_SIZE:-20G}"
RAID_SIZE="${RAID_SIZE:-4G}"

PIDFILE="run/qemu.pid"
SERIAL_SOCK="run/serial.sock"
QMP_SOCK="run/qmp.sock"
CONSOLE_LOG="logs/console.log"

# -------------------------------------------------------------------
# Sanity: do not start a second VM on top of a running one.
# -------------------------------------------------------------------
if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    echo "launch-dfly: VM already running (pid $(cat "$PIDFILE"))" >&2
    echo "             stop it first: bin/qmp quit" >&2
    exit 1
fi

# Stale sockets / pid
rm -f "$PIDFILE" "$SERIAL_SOCK" "$QMP_SOCK"

# -------------------------------------------------------------------
# Resolve mode-specific args
# -------------------------------------------------------------------
CDROM_ARGS=()
BOOT_ARGS=()

case "$MODE" in
    reset)
        if [ ! -f "$BASE" ]; then
            echo "launch-dfly: no base image at $BASE — run install first" >&2
            exit 1
        fi
        rm -f "$SYS"
        qemu-img create -f qcow2 -F qcow2 -b "$(realpath "$BASE")" "$SYS"
        echo "launch-dfly: overlay reset from base"
        exit 0
        ;;
    install)
        ISO="${ISO:-$(ls iso/dfly-*REL*.iso 2>/dev/null | head -1)}"
        if [ -z "$ISO" ] || [ ! -f "$ISO" ]; then
            echo "launch-dfly: no installer ISO found in iso/ (set ISO=...)" >&2
            exit 1
        fi
        # Use a plain (no-backing) system disk for the install.
        if [ ! -f "$SYS" ]; then
            qemu-img create -f qcow2 "$SYS" "$SYS_SIZE"
        fi
        CDROM_ARGS=(-cdrom "$ISO")
        BOOT_ARGS=(-boot d)
        echo "launch-dfly: install mode, CDROM=$ISO"
        ;;
    run)
        if [ ! -f "$SYS" ]; then
            if [ -f "$BASE" ]; then
                qemu-img create -f qcow2 -F qcow2 -b "$(realpath "$BASE")" "$SYS"
                echo "launch-dfly: created overlay from base"
            else
                echo "launch-dfly: no system disk at $SYS and no base at $BASE" >&2
                echo "             run: ./launch-dfly.sh install" >&2
                exit 1
            fi
        fi
        BOOT_ARGS=(-boot c)
        ;;
    *)
        echo "usage: $0 {run|install|reset}" >&2
        exit 2
        ;;
esac

# -------------------------------------------------------------------
# RAID test disks
# -------------------------------------------------------------------
RAID_ARGS=()
for i in $(seq 0 $((NDISKS - 1))); do
    img="images/raid${i}.qcow2"
    if [ ! -f "$img" ]; then
        qemu-img create -f qcow2 "$img" "$RAID_SIZE" >/dev/null
        echo "launch-dfly: created $img ($RAID_SIZE)"
    fi
    RAID_ARGS+=(
        -drive "if=none,id=raid${i},format=qcow2,file=${img},cache=writeback"
        -device "virtio-blk-pci,drive=raid${i},serial=RAID${i}"
    )
done

# -------------------------------------------------------------------
# Truncate console log per launch (panic forensics belong to the run
# that produced them; older runs are preserved in logs/runs/).
# -------------------------------------------------------------------
: > "$CONSOLE_LOG"

# -------------------------------------------------------------------
# Build the QEMU command
# -------------------------------------------------------------------
# Install mode needs VGA + GTK because DragonFly's dfuiinstaller spawns
# dfuife_curses on a separate VT — there is no such VT on a serial-only
# guest, so the backend stalls forever waiting for a frontend. The
# graphical window only appears during install; the installed system
# uses serial via /boot/loader.conf (see harness/guest-config/apply.sh).
DISPLAY_ARGS=(-nographic)
if [ "$MODE" = "install" ]; then
    DISPLAY_ARGS=(-vga std -display gtk,window-close=off)
fi

QEMU_ARGS=(
    -name h2dev
    -enable-kvm
    -cpu host
    -smp "$CPUS"
    -m "$RAM"
    "${DISPLAY_ARGS[@]}"
    -nodefaults

    # System disk (virtio-blk for vtbd0)
    -drive "if=none,id=sys,format=qcow2,file=${SYS},cache=writeback"
    -device "virtio-blk-pci,drive=sys,serial=SYS,bootindex=1"

    "${RAID_ARGS[@]}"

    # User-mode net with SSH port forward
    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:${SSH_PORT}-:22"
    -device "virtio-net-pci,netdev=net0"

    # Serial console → Unix socket + logfile
    -chardev "socket,id=ser,path=${SERIAL_SOCK},server=on,wait=off,logfile=${CONSOLE_LOG},logappend=on"
    -serial "chardev:ser"

    # QMP control socket
    -chardev "socket,id=mon,path=${QMP_SOCK},server=on,wait=off"
    -mon "chardev=mon,mode=control"

    -pidfile "$PIDFILE"

    "${CDROM_ARGS[@]}"
    "${BOOT_ARGS[@]}"
)

if [ "${FOREGROUND:-0}" = "1" ]; then
    echo "launch-dfly: starting QEMU in foreground (FOREGROUND=1)"
    exec qemu-system-x86_64 "${QEMU_ARGS[@]}"
else
    QEMU_ARGS+=(-daemonize)
    qemu-system-x86_64 "${QEMU_ARGS[@]}"
    echo "launch-dfly: VM started, pid $(cat "$PIDFILE")"
    echo "             console:  bin/console follow"
    echo "             monitor:  bin/qmp query-status"
    echo "             shell:    ssh h2dev"
    echo "             stop:     bin/qmp quit"
fi
