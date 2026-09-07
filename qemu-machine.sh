# qemu-machine.sh — the one definition of the h2dev QEMU machine.
#
# Sourced by launch-dfly.sh (one-shot, daemonized, plus install mode) and by
# host-run.sh (supervision loop out on the host). Everything that defines the
# machine itself lives here so the two cannot drift: the callers differ only in
# display wiring and in whether QEMU is daemonized.
#
# Requires bash (arrays). Callers must cd to the harness root first.
#
# Environment overrides, all optional:
#   NDISKS=4      number of RAID6 test disks
#   RAM=4G        guest memory
#   CPUS=4        vCPUs
#   SSH_PORT=2322 host port forwarded to guest :22
#   SYS_SIZE=20G  system disk size when creating a blank one (install mode)
#   RAID_SIZE=4G  size of each RAID test disk
#   H2_DISPLAY_ARGS  array; defaults to -nographic

NDISKS="${NDISKS:-4}"
RAM="${RAM:-4G}"
CPUS="${CPUS:-4}"
SSH_PORT="${SSH_PORT:-2322}"

BASE="${BASE:-images/base-dfly-6.4.2.qcow2}"
SYS="${SYS:-images/overlay-system.qcow2}"
SYS_SIZE="${SYS_SIZE:-20G}"
RAID_SIZE="${RAID_SIZE:-4G}"

PIDFILE="run/qemu.pid"
SUPERVISOR_PIDFILE="run/supervisor.pid"
SERIAL_SOCK="run/serial.sock"
QMP_SOCK="run/qmp.sock"
CONSOLE_LOG="logs/console.log"

mkdir -p run logs images iso

# KVM when the host offers it, TCG when it does not. TCG is roughly 10x slower
# — 8-12 minutes to sshd rather than under a minute — but it is the difference
# between testing on DragonFly and not testing at all; the claude-box sandbox
# has no /dev/kvm and cannot be given one without widening the jail. That is
# also the reason host-run.sh exists: run QEMU out on the host where the device
# works, and drive it from the box over unix sockets.
h2_accel_args() {
    ACCEL_ARGS=(-enable-kvm -cpu host)
    if [ ! -c /dev/kvm ] || [ ! -w /dev/kvm ]; then
        echo "qemu-machine: no usable /dev/kvm, falling back to TCG (slow)" >&2
        ACCEL_ARGS=(-accel "tcg,thread=multi" -cpu qemu64)
    fi
}

# Thin overlay on the locked base image. Cheap to throw away; the base is
# never dirtied.
h2_ensure_sys() {
    if [ ! -f "$SYS" ]; then
        if [ ! -f "$BASE" ]; then
            echo "qemu-machine: no system disk at $SYS and no base at $BASE" >&2
            echo "              run: ./launch-dfly.sh install  (see harness/INSTALL.md)" >&2
            return 1
        fi
        qemu-img create -f qcow2 -F qcow2 -b "$(realpath "$BASE")" "$SYS" >/dev/null
        echo "qemu-machine: created overlay from base"
    fi
}

h2_ensure_raid_disks() {
    local i img
    RAID_ARGS=()
    for i in $(seq 0 $((NDISKS - 1))); do
        img="images/raid${i}.qcow2"
        if [ ! -f "$img" ]; then
            qemu-img create -f qcow2 "$img" "$RAID_SIZE" >/dev/null
            echo "qemu-machine: created $img ($RAID_SIZE)"
        fi
        RAID_ARGS+=(
            -drive "if=none,id=raid${i},format=qcow2,file=${img},cache=writeback"
            -device "virtio-blk-pci,drive=raid${i},serial=RAID${i}"
        )
    done
}

# Populates QEMU_ARGS. Serial console goes to a unix socket *and* a log file,
# QMP to a second socket: those three paths are the whole control plane, and
# they are ordinary files in this directory, so they work identically on the
# host and inside the sandbox.
h2_machine_args() {
    h2_accel_args
    h2_ensure_raid_disks

    local display=("${H2_DISPLAY_ARGS[@]:--nographic}")

    QEMU_ARGS=(
        -name h2dev
        "${ACCEL_ARGS[@]}"
        -smp "$CPUS"
        -m "$RAM"
        "${display[@]}"
        -nodefaults

        # System disk (virtio-blk for vtbd0)
        -drive "if=none,id=sys,format=qcow2,file=${SYS},cache=writeback"
        -device "virtio-blk-pci,drive=sys,serial=SYS,bootindex=1"

        "${RAID_ARGS[@]}"

        # User-mode net with SSH port forward
        -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:${SSH_PORT}-:22"
        -device "virtio-net-pci,netdev=net0"

        # Serial console -> unix socket + logfile
        -chardev "socket,id=ser,path=${SERIAL_SOCK},server=on,wait=off,logfile=${CONSOLE_LOG},logappend=on"
        -serial "chardev:ser"

        # QMP control socket
        -chardev "socket,id=mon,path=${QMP_SOCK},server=on,wait=off"
        -mon "chardev=mon,mode=control"

        -pidfile "$PIDFILE"
    )
}

# True when a QEMU from this harness is alive.
h2_running() {
    [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null
}

# True when host-run.sh is supervising. It matters because a supervisor brings
# QEMU straight back after a quit: anything that wants the disk images to
# itself (an overlay reset, an install) has to stop the supervisor first.
h2_supervised() {
    [ -f "$SUPERVISOR_PIDFILE" ] && \
        kill -0 "$(cat "$SUPERVISOR_PIDFILE" 2>/dev/null)" 2>/dev/null
}
