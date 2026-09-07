#!/bin/sh
# scp.sh — copy files to/from the guest with the harness identity.
#
# Write the guest side as `vm:` and it is expanded to root@127.0.0.1 with the
# right port and key:
#
#   ./scp.sh local_file.c vm:/usr/src/sys/vfs/hammer2/
#   ./scp.sh vm:/tmp/hammer2_raid6_full.patch ./hammer2_raid6.patch
#
# For whole trees use bin/push and bin/pull instead — they are rsync and know
# what to exclude.
set -e
VM_ROOT="$(cd "$(dirname "$0")" && pwd)"
. "$VM_ROOT/vmenv.sh"

vm_identity_check

args=""
for a in "$@"; do
	case "$a" in
		vm:*) a="$VM_TARGET:${a#vm:}" ;;
	esac
	args="$args $(printf '%s' "$a" | sed "s/'/'\\\\''/g; s/^/'/; s/\$/'/")"
done

# shellcheck disable=SC2086,SC2090  # both option lists must word-split
eval "scp $VM_SCP_OPTS $args"
