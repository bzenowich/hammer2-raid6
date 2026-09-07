#!/bin/sh
# Run a command on the DragonFlyBSD VM. Kept as the name older docs and
# scripts use; ./ssh.sh is the real thing and takes the same arguments.
#
# The old DFLY_IP default (192.168.25.66) was a LAN-bridged VM that no longer
# exists — host, port and key now come from vmenv.sh. Set DFLY_IP only if you
# really do have some other DragonFly box to talk to.
DIR="$(cd "$(dirname "$0")" && pwd)"

if [ -n "${DFLY_IP:-}" ]; then
	exec ssh root@"$DFLY_IP" "$@"
fi
exec "$DIR/ssh.sh" "$@"
