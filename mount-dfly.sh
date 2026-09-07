#!/bin/sh
# Mount the VM's root filesystem at mnt/dfly via sshfs.
# Unmount with: fusermount -u mnt/dfly
set -e
VM_ROOT="$(cd "$(dirname "$0")" && pwd)"
. "$VM_ROOT/vmenv.sh"

mkdir -p "$VM_ROOT/mnt/dfly"
# sshfs takes ssh's own options through -o ssh_command, which is the only way
# to pass the port and identity vmenv.sh resolved.
exec sshfs -o "ssh_command=ssh $VM_SSH_OPTS" \
	"$VM_TARGET":/ "$VM_ROOT/mnt/dfly"
