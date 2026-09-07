# vmenv.sh — how the harness reaches the h2dev guest over SSH.
#
# Sourced by ssh.sh, vmctl.sh, bin/push and bin/pull. POSIX sh, no bashisms.
#
# The point of this file is that nothing in the harness depends on a `h2dev`
# entry in ~/.ssh/config any more. The claude-box sandbox has no ~/.ssh at all
# — not the config, not the key — so every tool that said `ssh h2dev` was dead
# inside the box. Here the host, port and identity are resolved from the
# project, and the alias is only the last fallback.
#
# Identity, in order:
#   $H2_VM_KEY              explicit override
#   harness/id_ed25519      project-local key (gitignored; see harness/README.md)
#   ~/.ssh/h2dev_ed25519    the key the base image was built with, on the host
#   (none)                  fall through to ssh-agent / ~/.ssh/config
#
# The guest's login shell is tcsh, so a bare `ssh h2dev '<sh script>'` dies on
# sh syntax ("Illegal variable name"). vm_rsh() pipes the script to sh in the
# guest, which is the only reliable way to run one.

VM_USER="${VM_USER:-root}"
VM_HOST="${VM_HOST:-127.0.0.1}"
VM_PORT="${SSH_PORT:-2322}"
VM_TARGET="$VM_USER@$VM_HOST"

# Harness root, whichever bin/ or top-level script sourced this.
VM_ROOT="${VM_ROOT:-$(cd "$(dirname "$0")" && pwd)}"
case "$VM_ROOT" in */bin) VM_ROOT="${VM_ROOT%/bin}" ;; esac

mkdir -p "$VM_ROOT/run"   # known_hosts lives here

VM_KEY="${H2_VM_KEY:-}"
if [ -z "$VM_KEY" ]; then
	if [ -f "$VM_ROOT/harness/id_ed25519" ]; then
		VM_KEY="$VM_ROOT/harness/id_ed25519"
	elif [ -f "$HOME/.ssh/h2dev_ed25519" ]; then
		VM_KEY="$HOME/.ssh/h2dev_ed25519"
	fi
fi

# Host-key churn is normal here: the guest key lives in the locked base image,
# so it survives overlay resets, but re-baking the base changes it. Keeping
# known_hosts under run/ means `rm run/known_hosts` is the whole fix, and a
# stale entry never touches your personal known_hosts.
VM_SSH_BASE="-o StrictHostKeyChecking=accept-new"
VM_SSH_BASE="$VM_SSH_BASE -o UserKnownHostsFile=$VM_ROOT/run/known_hosts"
VM_SSH_BASE="$VM_SSH_BASE -o ConnectTimeout=${VM_CONNECT_TIMEOUT:-10}"
VM_IDENTITY=ok
if [ -n "$VM_KEY" ]; then
	VM_SSH_BASE="$VM_SSH_BASE -i $VM_KEY -o IdentitiesOnly=yes"
elif [ ! -S "${SSH_AUTH_SOCK:-}" ]; then
	# No key file and no usable agent — note that SSH_AUTH_SOCK is often set
	# but dangling inside the sandbox, so the variable alone proves nothing.
	# The only thing left that could work is a ~/.ssh/config entry, which is
	# exactly what does not exist in there.
	VM_IDENTITY=none
fi

# Warned about at most once per process, and only when something actually
# tries to reach the guest — `vmctl.sh log` has no business complaining.
vm_identity_check() {
	[ "$VM_IDENTITY" = none ] || return 0
	VM_IDENTITY=warned
	echo "vmenv: no SSH identity — set H2_VM_KEY, drop the key at" >&2
	echo "       $VM_ROOT/harness/id_ed25519, or run an agent." >&2
	echo "       (see harness/README.md, 'SSH identity')" >&2
}

# ssh takes -p, scp takes -P; everything else is shared.
VM_SSH_OPTS="-p $VM_PORT $VM_SSH_BASE"
VM_SCP_OPTS="-P $VM_PORT $VM_SSH_BASE"

# rsync's -e argument.
VM_RSYNC_RSH="ssh $VM_SSH_OPTS"

# Raw ssh: interactive shells, scp-style pass-through, liveness probes.
# shellcheck disable=SC2086  # VM_SSH_OPTS must word-split
vm_ssh() { vm_identity_check; ssh $VM_SSH_OPTS "$VM_TARGET" "$@"; }

# Run an sh script in the guest, past the tcsh login shell.
vm_rsh() { printf '%s\n' "$*" | vm_ssh sh; }

# Liveness. Quiet, bounded, and the only honest "is the guest up" test —
# QMP reports the CPU running long before DragonFly has finished booting.
#
# The overrides go BEFORE $VM_SSH_OPTS on purpose: ssh keeps the *first* value
# it is given for an option, so a later -o would lose to the base ConnectTimeout.
vm_up() {
	vm_identity_check
	timeout 10 ssh -o ConnectTimeout=5 -o BatchMode=yes $VM_SSH_OPTS \
		"$VM_TARGET" true >/dev/null 2>&1
}
