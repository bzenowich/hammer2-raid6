#!/bin/sh
# Run a command on the DragonFlyBSD VM
# Set DFLY_IP in your environment, or edit the default below
DFLY_IP="${DFLY_IP:-192.168.25.102}"
ssh root@"$DFLY_IP" "$@"
