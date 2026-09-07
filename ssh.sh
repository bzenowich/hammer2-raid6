#!/bin/sh
# ssh.sh — a shell on the h2dev guest, or one command on it.
#
#   ./ssh.sh                     interactive login shell (tcsh)
#   ./ssh.sh 'cmd; cmd'          run an sh script line in the guest
#   ./ssh.sh --raw <args...>     hand the arguments straight to ssh
#   ./ssh.sh --raw -L 8443:localhost:443 -N     (tunnels, scp -o..., etc.)
#
# The guest's login shell is tcsh, so the default form pipes the command to sh
# rather than letting tcsh parse it. `--raw` is the escape hatch for anything
# that needs ssh's own argument handling — tunnels, or a command reading stdin.
set -e
VM_ROOT="$(cd "$(dirname "$0")" && pwd)"
. "$VM_ROOT/vmenv.sh"

case "${1:-}" in
	"")     vm_ssh ;;
	--raw)  shift; vm_ssh "$@" ;;
	*)      vm_rsh "$*" ;;
esac
