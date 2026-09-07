#!/bin/bash
# host-run.sh — supervise the h2dev VM on the HOST, where /dev/kvm exists.
#
# Why this exists: development happens inside a claude-box bwrap jail with no
# /dev/kvm, which cannot be handed one without widening the sandbox. A VM
# booted in there falls back to TCG and takes 8-12 minutes to reach a login,
# every single time. QEMU runs out here instead and publishes its whole control
# plane as files in this directory — which IS mounted into the box:
#
#   run/qmp.sock       lifecycle control (reset, snapshots, quit)
#   run/serial.sock    live serial console, attachable
#   logs/console.log   every byte the console printed, for reading boot progress
#
# ./vmctl.sh is the front end for all three and behaves the same on the host
# and in the box.
#
# The loop is what turns "quit" into a cold boot: `./vmctl.sh coldboot` asks
# QEMU to exit and this brings it straight back up on a fresh process. So the
# full lifecycle is reachable from inside the sandbox without the device.
#
# Stop the supervisor with Ctrl-C; the guest console is not on this terminal,
# so Ctrl-C reaches this shell rather than the guest.
#
# Environment overrides are the same as launch-dfly.sh (NDISKS, RAM, CPUS,
# SSH_PORT, ...). Extra arguments are passed through to QEMU.

set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

. ./qemu-machine.sh

if h2_supervised; then
    echo "host-run: already supervising (pid $(cat "$SUPERVISOR_PIDFILE"))" >&2
    exit 1
fi
if h2_running; then
    echo "host-run: a VM is already running (pid $(cat "$PIDFILE"))" >&2
    echo "          stop it first: ./vmctl.sh quit" >&2
    exit 1
fi

h2_ensure_sys

if [ ! -c /dev/kvm ] || [ ! -w /dev/kvm ]; then
    cat >&2 <<'WARN'
warning: /dev/kvm is not usable here, so this supervisor buys no speed over
         ./launch-dfly.sh — both fall back to TCG. It still buys the rest:
         restart-on-exit, cold boot from inside the sandbox, and a control
         plane that outlives this terminal. Check that the device node exists
         and that this uid can open it.
WARN
fi

# Stale sockets from a killed run would make QEMU fail to bind.
rm -f "$QMP_SOCK" "$SERIAL_SOCK" "$PIDFILE"

# One console log for the whole supervised session: QEMU appends, and a cold
# boot in the middle of a run must not erase what led up to it.
: > "$CONSOLE_LOG"

# The pidfile is how everything else knows a supervisor is in charge — a quit
# here means "cold boot", not "shut down", and an overlay reset underneath a
# live QEMU would corrupt the run.
echo $$ > "$SUPERVISOR_PIDFILE"
trap 'rm -f "$SUPERVISOR_PIDFILE"' EXIT

# QEMU runs as a background job and we wait(1) on it, rather than running it in
# the foreground: bash defers a trap until the foreground child exits, so
# `kill $(cat run/supervisor.pid)` would be ignored for as long as the guest
# lived — and then leave that guest orphaned. `wait` is interruptible, so the
# handler runs immediately and takes QEMU down with it.
stopping=0
on_signal() {
    stopping=1
    [ -n "${qemu_pid:-}" ] && kill "$qemu_pid" 2>/dev/null
    return 0
}
trap on_signal INT TERM

while :; do
    echo "host-run: starting QEMU"
    h2_machine_args
    status=0
    qemu-system-x86_64 "${QEMU_ARGS[@]}" -boot c "$@" &
    qemu_pid=$!
    wait "$qemu_pid" || status=$?
    qemu_pid=""
    rm -f "$QMP_SOCK" "$SERIAL_SOCK" "$PIDFILE"
    if [ "$stopping" -eq 1 ]; then
        echo
        echo "host-run: stopped"
        exit 0
    fi
    # A nonzero exit is a QEMU failure rather than a requested shutdown —
    # usually a bad argument or a busy port, which restarting will not fix.
    # Pause so the loop cannot become a spin.
    if [ "$status" -ne 0 ]; then
        echo "host-run: QEMU exited $status — pausing 5s before restart (Ctrl-C to stop)" >&2
        sleep 5
    else
        echo "host-run: QEMU exited — restarting (Ctrl-C to stop)"
    fi
done
