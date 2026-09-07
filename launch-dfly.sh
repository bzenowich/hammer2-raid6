#!/bin/bash
# launch-dfly.sh — start the harness VM as a one-shot QEMU.
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
#
# The machine itself is defined in qemu-machine.sh, shared with host-run.sh.
#
# This is the direct way: one QEMU, gone when it exits. When something *else*
# has to boot and reboot the VM — an agent in the claude-box sandbox above all
# — use ./host-run.sh on the host instead and drive it with ./vmctl.sh.

set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

MODE="${1:-run}"

. ./qemu-machine.sh

# -------------------------------------------------------------------
# Sanity: do not start a second VM on top of a running one, and never touch
# the disk images while host-run.sh is supervising — it would boot a fresh
# QEMU straight back onto them.
# -------------------------------------------------------------------
if h2_supervised; then
    echo "launch-dfly: ./host-run.sh is supervising the VM (pid $(cat "$SUPERVISOR_PIDFILE"))" >&2
    echo "             stop it first, or drive the running VM with ./vmctl.sh" >&2
    exit 1
fi

if [ "$MODE" != "reset" ] && h2_running; then
    echo "launch-dfly: VM already running (pid $(cat "$PIDFILE"))" >&2
    echo "             stop it first: ./vmctl.sh quit" >&2
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
        # Install mode needs VGA + GTK because DragonFly's dfuiinstaller spawns
        # dfuife_curses on a separate VT — there is no such VT on a serial-only
        # guest, so the backend stalls forever waiting for a frontend. The
        # graphical window only appears during install; the installed system
        # uses serial via /boot/loader.conf (harness/guest-config/apply.sh).
        H2_DISPLAY_ARGS=(-vga std -display gtk,window-close=off)
        echo "launch-dfly: install mode, CDROM=$ISO"
        ;;
    run)
        h2_ensure_sys
        BOOT_ARGS=(-boot c)
        ;;
    *)
        echo "usage: $0 {run|install|reset}" >&2
        exit 2
        ;;
esac

# Truncate console log per launch (panic forensics belong to the run that
# produced them; older runs are preserved in logs/runs/).
: > "$CONSOLE_LOG"

h2_machine_args
QEMU_ARGS+=("${CDROM_ARGS[@]}" "${BOOT_ARGS[@]}")

if [ "${FOREGROUND:-0}" = "1" ]; then
    echo "launch-dfly: starting QEMU in foreground (FOREGROUND=1)"
    exec qemu-system-x86_64 "${QEMU_ARGS[@]}"
else
    QEMU_ARGS+=(-daemonize)
    qemu-system-x86_64 "${QEMU_ARGS[@]}"
    echo "launch-dfly: VM started, pid $(cat "$PIDFILE")"
    echo "             wait:     ./vmctl.sh wait"
    echo "             console:  ./vmctl.sh log   /  ./vmctl.sh console"
    echo "             status:   ./vmctl.sh status"
    echo "             shell:    ./ssh.sh"
    echo "             stop:     ./vmctl.sh quit"
fi
