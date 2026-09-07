# The h2dev VM harness

A DragonFlyBSD 6.4.2 guest under QEMU, headless, driven entirely from the
shell. It is the only place the RAID6 kernel work can be built, panicked and
recovered — and it is also the dev box for the sibling `../dfly` (FlyNAS)
project, which deploys to it.

```
./host-run.sh          # on the HOST, once: supervise a long-lived VM at KVM speed
./vmctl.sh             # drive it — wait, reset, coldboot, snapshots, console, log
./ssh.sh               # root shell, or ./ssh.sh 'command'
./scp.sh f vm:/path    # copy one file either way (`vm:` is the guest)
./launch-dfly.sh run   # or: one-shot VM, no supervisor, gone when it exits
./launch-dfly.sh reset # discard the overlay, recreate it from the locked base
bin/push  bin/pull     # rsync the tree in, crash dumps and artifacts out
./mount-dfly.sh        # sshfs the guest's root at mnt/dfly
bin/run-test <script>  # full reset → boot → push → test → collect cycle
bin/watcher start      # panic/lockup detection and recovery
```

One-time base-image install: **`INSTALL.md`**. Everything below assumes
`images/base-dfly-6.4.2.qcow2` already exists.

## Interactive, or supervised

`launch-dfly.sh` is the direct way: one QEMU, daemonized, gone when it exits.
Use it when you are the one at the keyboard and nothing needs to reboot the
guest for you.

`host-run.sh` is for when something *else* has to boot and reboot the VM — an
agent working in the claude-box sandbox, most of all. That box is
`--unshare-all` with a minimal `/dev`, so it has no `/dev/kvm` and cannot be
given one without widening the jail; anything it boots itself falls back to TCG
and takes **8-12 minutes** to reach a login. `host-run.sh` therefore runs QEMU
out on the host, where KVM works, and publishes its channels as files in this
project — which the box already mounts:

```
run/qmp.sock       lifecycle control (reset, snapshots, quit)
run/serial.sock    live serial console, attachable
logs/console.log   everything the console has printed, for reading boot progress
```

`vmctl.sh` is the front end for all three and behaves the same on the host and
inside the box. Because `host-run.sh` is a supervision *loop*, `vmctl.sh
coldboot` can ask QEMU to quit and get a fresh one back — so the whole
lifecycle is reachable without the device:

```
./host-run.sh &                 # on the HOST, once
./vmctl.sh wait                 # block until the guest answers SSH
./vmctl.sh save booted          # snapshot the booted state, once
./vmctl.sh load booted          # ...and from then on, restore in seconds
./vmctl.sh reset                # hard reset, no firmware re-init
./vmctl.sh coldboot             # quit QEMU; the loop boots it fresh
./vmctl.sh nmi                  # break a wedged kernel into ddb
./vmctl.sh log 60               # last 60 lines of console output
./vmctl.sh console              # attach to the serial console (detach: C-])
```

The machine itself — disks, NICs, console and QMP wiring — is defined once in
`qemu-machine.sh`, which both launchers source, so the supervised and one-shot
VMs cannot drift apart.

**The snapshot workflow is the point under TCG.** `save`/`load` write and read
guest RAM into the qcow2 disks; a `load` takes seconds where a boot takes ten
minutes. Boot once, `save booted`, and stop booting.

**One supervisor at a time, and it owns the disks.** `host-run.sh` writes
`run/supervisor.pid`. While it is alive, `launch-dfly.sh` and `bin/run-test`
refuse to run: both want to delete and recreate the overlay, and a supervisor
would boot a fresh QEMU straight back onto the images mid-rewrite. Stop the
supervisor, or get a clean state with `./vmctl.sh load <snapshot>` instead.

## Reaching the guest from the sandbox

Two things are separate: the *control plane* (sockets and logs in this
directory — always reachable, nothing to configure) and the guest's *ports*.

`ssh.sh`, `bin/push` and `../dfly/bin/deploy` dial `127.0.0.1:2322`, and the
box's loopback is its own, so `sandbox.conf` carries a `net` bridge to the
host's. It is raw TCP and not covered by the egress allowlist — see the note
there. A `sandbox.conf` edit only takes effect on the **next** claude-box
launch; a session cannot loosen the box it is already inside.

That bridge also occupies port 2322 inside the box, so a VM launched *in* the
box must use another port: `SSH_PORT=2323 ./launch-dfly.sh run`.

## SSH identity

`vmenv.sh` resolves host, port and key for every tool here, in this order:

1. `$H2_VM_KEY`
2. `harness/id_ed25519` — project-local, gitignored
3. `~/.ssh/h2dev_ed25519` — the key the base image was built with
4. otherwise, whatever an `ssh-agent` offers

Nothing depends on a `h2dev` entry in `~/.ssh/config` any more. That matters
because **the sandbox has no `~/.ssh` at all** — not the config, not the key —
so every tool that said `ssh h2dev` was dead in there. To use the harness from
inside the box, either copy the key to `harness/id_ed25519` (it is under the
project, which is mounted, and it is gitignored), or uncomment the
`ssh ~/.ssh/h2dev_ed25519` line in `sandbox.conf` to lend the box that one key
through an agent without exposing the file.

The guest's public key is baked into the base image
(`harness/guest-config/authorized_keys`), so a new key means re-baking the base
— copying the existing private key is the cheap path.

Host keys are recorded in `run/known_hosts`, never in your personal one. After
re-baking the base image the guest's host key changes: `rm run/known_hosts`.

## The guest's shell is tcsh

`ssh h2dev '<sh script>'` fails with `Illegal variable name` on anything with
sh syntax. `./ssh.sh 'cmd; cmd'` pipes the script to `sh` in the guest, which
is the only reliable form. `./ssh.sh --raw ...` hands arguments straight to ssh
for tunnels and anything that reads stdin.

## Reading state

| Want | Command |
|---|---|
| Is it up? | `./vmctl.sh status` |
| Where did the boot stop? | `./vmctl.sh log 80` |
| Did it panic? | `./vmctl.sh grep 'panic:'` |
| Drive a ddb prompt | `./vmctl.sh console` |
| Crash dumps on the host | `bin/pull` → `logs/dumps/` |
| Last test run | `logs/runs/<ts>/result.txt` |

`bin/watcher start` automates the panic case: it polls SSH, classifies what the
console shows (`panic_reboot`, `panic_ddb`, `lockup`, `qemu_gone`), captures
state to `logs/panics/<ts>/`, and resets or waits as appropriate. The design
rationale for all of it is `../docs/harness.md`.
