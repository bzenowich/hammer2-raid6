#!/bin/bash
# vmctl.sh — drive the h2dev VM, whoever started it.
#
# Every channel it uses is a unix socket or a file under run/ and logs/, so it
# works unchanged inside the claude-box sandbox: the project is mounted there,
# and none of this needs /dev/kvm. With ./host-run.sh supervising on the host,
# QEMU stays out there with KVM and the whole lifecycle is still reachable from
# in here — including a cold boot.
#
#   ./vmctl.sh status          running? and what the guest CPU is doing
#   ./vmctl.sh wait [SECS]     block until the guest answers SSH (default 900)
#   ./vmctl.sh reset [SECS]    hard reset — the fast reboot, no firmware re-init
#   ./vmctl.sh reboot [SECS]   graceful reboot over SSH, then wait for it back
#   ./vmctl.sh coldboot [SECS] quit QEMU; ./host-run.sh boots it fresh
#   ./vmctl.sh quit            stop QEMU (a supervisor would restart it)
#   ./vmctl.sh nmi             inject an NMI — breaks a wedged kernel into ddb
#   ./vmctl.sh save NAME       snapshot the live VM into the qcow2 disks
#   ./vmctl.sh load NAME [SECS]  restore that snapshot — seconds, vs a full boot
#   ./vmctl.sh snapshots       list snapshots
#   ./vmctl.sh delsnap NAME    delete a snapshot
#   ./vmctl.sh console         attach to the serial console (detach: C-])
#   ./vmctl.sh log [N]         last N lines of console output (default 40)
#   ./vmctl.sh grep PATTERN    search the whole console log
#
# The workflow this is built for, and the reason it matters more here than on
# a KVM host: a TCG boot of DragonFly takes 8-12 minutes. Boot once, `save
# booted`, and from then on `load booted` instead of booting at all.

set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

. ./qemu-machine.sh   # PIDFILE, QMP_SOCK, SERIAL_SOCK, CONSOLE_LOG, h2_running
. ./vmenv.sh          # vm_up, vm_ssh, vm_rsh

qmp() { bin/qmp "$@"; }

# The monitor reports failures (no such snapshot, no writable disk) as text on
# an otherwise successful QMP call, so its output has to be shown, not dropped.
monitor() {
	qmp human-monitor-command \
		"$(python3 -c 'import json,sys; print(json.dumps({"command-line": sys.argv[1]}))' "$1")"
}

require_running() {
	if ! h2_running; then
		echo "vmctl: no VM running (no live pid in $PIDFILE)" >&2
		echo "       start one:  ./host-run.sh   (on the host, supervised)" >&2
		echo "                   ./launch-dfly.sh run   (one-shot, here)" >&2
		exit 1
	fi
	[ -S "$QMP_SOCK" ] || {
		echo "vmctl: VM is running but $QMP_SOCK is missing — started without QMP?" >&2
		exit 1
	}
}

wait_for_ssh() {
	local deadline=$(( $(date +%s) + ${1:-900} ))
	while [ "$(date +%s)" -lt "$deadline" ]; do
		if vm_up; then
			echo "vmctl: up"
			return 0
		fi
		sleep 3
	done
	echo "vmctl: timed out waiting for SSH — ./vmctl.sh log to see where it stopped" >&2
	return 1
}

case "${1:-}" in
status)
	if h2_running; then
		echo "qemu: pid $(cat "$PIDFILE")"
		if [ -S "$QMP_SOCK" ]; then qmp query-status; fi
		if vm_up; then echo "ssh: up"; else echo "ssh: not answering"; fi
	else
		echo "not running (no live pid in $PIDFILE)"
		if [ -S "$QMP_SOCK" ]; then
			echo "note: stale $QMP_SOCK left by a previous QEMU"
		fi
		exit 1
	fi
	;;
wait)
	wait_for_ssh "${2:-900}"
	;;
reset)
	require_running
	qmp system_reset >/dev/null
	echo "vmctl: reset"
	wait_for_ssh "${2:-900}"
	;;
reboot)
	require_running
	# DragonFly drops the connection as it goes down, so ssh's exit status
	# here says nothing; the wait below is the real check.
	vm_rsh reboot >/dev/null 2>&1 || true
	echo "vmctl: reboot requested"
	sleep 5
	wait_for_ssh "${2:-900}"
	;;
coldboot)
	require_running
	h2_supervised || {
		echo "vmctl: nothing is supervising this VM, so a quit would just stop it." >&2
		echo "       Use ./vmctl.sh reset for a reboot, or run ./host-run.sh on the host." >&2
		exit 1
	}
	qmp quit >/dev/null 2>&1 || true
	# Wait for the socket to go, then come back: host-run.sh recreates it
	# when the next QEMU process starts.
	deadline=$(( $(date +%s) + 30 ))
	while [ -S "$QMP_SOCK" ] && [ "$(date +%s)" -lt "$deadline" ]; do sleep 1; done
	deadline=$(( $(date +%s) + 60 ))
	while [ ! -S "$QMP_SOCK" ]; do
		[ "$(date +%s)" -lt "$deadline" ] || {
			echo "vmctl: QEMU did not come back — is ./host-run.sh still running?" >&2
			exit 1
		}
		sleep 1
	done
	echo "vmctl: cold boot started"
	wait_for_ssh "${2:-1200}"
	;;
quit)
	require_running
	qmp quit >/dev/null 2>&1 || true
	if h2_supervised; then
		echo "vmctl: quit sent — host-run.sh will boot a fresh QEMU (that is coldboot)"
	else
		echo "vmctl: quit sent"
	fi
	;;
nmi)
	require_running
	qmp inject-nmi >/dev/null
	echo "vmctl: NMI injected — ./vmctl.sh console to reach the ddb prompt"
	;;
save)
	require_running
	[ -n "${2:-}" ] || { echo "usage: ./vmctl.sh save NAME" >&2; exit 1; }
	monitor "savevm $2"
	echo "vmctl: saved '$2'"
	;;
load)
	require_running
	[ -n "${2:-}" ] || { echo "usage: ./vmctl.sh load NAME" >&2; exit 1; }
	monitor "loadvm $2"
	echo "vmctl: loaded '$2'"
	wait_for_ssh "${3:-300}"
	;;
delsnap)
	require_running
	[ -n "${2:-}" ] || { echo "usage: ./vmctl.sh delsnap NAME" >&2; exit 1; }
	monitor "delvm $2"
	echo "vmctl: deleted '$2'"
	;;
snapshots)
	require_running
	monitor "info snapshots"
	;;
console)
	[ -S "$SERIAL_SOCK" ] || { echo "vmctl: no $SERIAL_SOCK — is the VM running?" >&2; exit 1; }
	command -v socat >/dev/null || { echo "vmctl: socat not installed" >&2; exit 1; }
	echo "vmctl: attaching to console — detach with C-]" >&2
	exec socat -,raw,echo=0,escape=0x1d "UNIX-CONNECT:$SERIAL_SOCK"
	;;
log)
	exec bin/console tail "${2:-40}"
	;;
grep)
	[ -n "${2:-}" ] || { echo "usage: ./vmctl.sh grep PATTERN" >&2; exit 1; }
	exec bin/console grep "$2"
	;;
*)
	grep '^#   \./vmctl\.sh' "$0" | sed 's/^#   //'
	exit 1
	;;
esac
