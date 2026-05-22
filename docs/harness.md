# Autonomous QEMU Harness for HAMMER2 RAID6 Development

**Date**: 2026-05-21
**Status**: Proposed
**Target guest**: DragonFlyBSD 6.4.2-RELEASE (released ~2026-05-11)

The goal of this harness is to let the development agent build, test, observe,
recover, and iterate on the guest kernel without requiring the human to take
screenshots, manually interrupt hung VMs, or drive the installer. Every guest
state — booting, running, panicking, deadlocked — must be programmatically
observable and controllable from the host shell.

## Design Principles

1. **No framebuffer, ever.** All guest output flows through serial. Panics,
   ddb prompts, getty, kernel boot messages — all to a text file on the host.
2. **Headless QEMU.** No GUI window. Detached, controllable via Unix sockets.
3. **Programmatic control plane.** QMP (JSON) over Unix socket. Reset, NMI,
   quit, status, all scriptable.
4. **rsync over SSH for host↔guest transfer.** DragonFlyBSD lacks
   virtio-9p / virtiofs. Rather than introduce host-side nfsd or smbd,
   use stock rsync: explicit push of source before a build, explicit
   pull of crash dumps and artifacts after. Two small wrappers
   (`bin/push`, `bin/pull`) make this a one-word step.
5. **Base + overlay disk images.** One clean post-install snapshot, never
   modified. Every test boot uses a thin overlay that resets in seconds.
6. **Active failure detection.** A watcher process polls SSH liveness and
   greps console for panic markers. Reacts within seconds — no human in
   the loop.
7. **No sudo, no bridge.** User-mode networking with SSH port forward.
   Trade LAN visibility for setup robustness.
8. **Deterministic reset.** A single script tears the VM down, restores
   the base image, brings it back up. Idempotent.

## Components

### Disk Layout

```
host:
  iso/dfly-6.4.2-RELEASE.iso         downloaded once, immutable
  images/
    base-dfly-6.4.2.qcow2            post-install snapshot, immutable
    overlay-system.qcow2             backing=base, working system disk
    raid0.qcow2 .. raidN.qcow2       RAID6 test disks (regenerated per test)
  logs/
    console.log                      every boot's serial output
    monitor.log                      QMP command history
    ssh.log                          ssh attempt log from watcher
    panics/                          per-panic captured logs + stack
    dumps/                           crash dumps pulled from guest /var/crash
    artifacts/                       test outputs pulled from guest
```

Overlay reset:
```
rm images/overlay-system.qcow2
qemu-img create -f qcow2 -b images/base-dfly-6.4.2.qcow2 \
                         -F qcow2 images/overlay-system.qcow2
```
~1 second. Boots clean.

### QEMU Invocation

```
qemu-system-x86_64 \
  -name h2dev -enable-kvm -cpu host -smp 4 -m 4G -nographic \
  \
  -drive if=virtio,id=sys,file=images/overlay-system.qcow2 \
  -drive if=virtio,id=disk0,file=images/raid0.qcow2,serial=RAID0 \
  ... (raid1..raidN) ... \
  \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 \
  -device virtio-net-pci,netdev=net0 \
  \
  -chardev socket,id=ser,path=run/serial.sock,server=on,wait=off,\
           logfile=logs/console.log,logappend=on \
  -serial chardev:ser \
  \
  -chardev socket,id=mon,path=run/qmp.sock,server=on,wait=off \
  -mon chardev=mon,mode=control \
  \
  -pidfile run/qemu.pid \
  -daemonize
```

Key choices:
- `-nographic`: no SDL/GTK window. Pure headless.
- `-serial chardev:ser`: kernel console → Unix socket → logfile on host.
- `-mon chardev=mon,mode=control`: QMP (JSON), not HMP. Scriptable.
- `-netdev user,...hostfwd=`: SSH on host port 2222. No bridge. No sudo.
- No `-fsdev` / `-virtio-9p`: DragonFlyBSD has no 9p client. Source
  transfer is rsync over SSH (see `bin/push` / `bin/pull` below).
- `-daemonize`: QEMU returns to shell; control via sockets only.
- No `-display`, no `-vga`: removes the only reason screenshots were ever needed.

### Guest Configuration (one-time, baked into base image)

`/boot/loader.conf`:
```
console="comconsole,vidconsole"     # serial primary
comconsole_speed="115200"
boot_serial="YES"
hint.sio.0.flags="0x10"             # console-eligible
debug.acpi.disabled=""              # leave default
```

`/etc/ttys`:
```
ttyu0   "/usr/libexec/getty std.115200"   xterm   on  secure
```
(boot to a serial login on COM1)

`/etc/rc.conf`:
```
hostname="h2dev"
sshd_enable="YES"
dumpdev="AUTO"                      # crash dump to swap
```

`/etc/sysctl.conf`:
```
debug.bootverbose=1
debug.debugger_on_panic=0           # auto-reboot, dump to swap
machdep.panic_reboot_wait_time=5
```

Effect: on panic, guest writes a kernel dump to swap, reboots after 5 s,
`savecore` on next boot copies the dump to `/var/crash` on the guest's
local disk. Watcher pulls new dumps to host `logs/dumps/` via rsync
when it detects a post-panic SSH recovery.

`/root/.ssh/authorized_keys`: contains the host agent's public key.

### SSH Configuration (host)

`~/.ssh/config` snippet:
```
Host h2dev
  HostName 127.0.0.1
  Port 2222
  User root
  IdentityFile ~/.ssh/h2dev_ed25519
  StrictHostKeyChecking no
  UserKnownHostsFile /dev/null
  ControlMaster auto
  ControlPath /tmp/ssh-h2dev-%r
  ControlPersist 60s
  ConnectTimeout 5
```

`ControlPersist` keeps a multiplexed connection alive — `ssh h2dev <cmd>`
becomes effectively free after the first call.

### QMP Helper

A tiny shell wrapper, `bin/qmp`:
```
#!/bin/sh
# qmp <command> [<arg-json>]
# Examples:
#   qmp query-status
#   qmp system_reset
#   qmp inject-nmi
#   qmp quit
exec python3 - "$@" <<'PY'
import socket, json, sys
s = socket.socket(socket.AF_UNIX); s.connect("run/qmp.sock")
def recv():
    buf = b""
    while not buf.endswith(b"\n"): buf += s.recv(4096)
    return json.loads(buf.decode())
recv()                              # greeting
s.sendall(b'{"execute":"qmp_capabilities"}\n'); recv()
cmd = {"execute": sys.argv[1]}
if len(sys.argv) > 2: cmd["arguments"] = json.loads(sys.argv[2])
s.sendall((json.dumps(cmd)+"\n").encode())
print(json.dumps(recv(), indent=2))
PY
```

Operations the agent uses:
- `qmp query-status` — running, paused, internal-error?
- `qmp system_reset` — hard reset (lockup recovery).
- `qmp inject-nmi` — break into ddb if it's running.
- `qmp quit` — clean shutdown of QEMU.
- `qmp stop` / `qmp cont` — pause / resume.

### Console Reader

The serial logfile (`logs/console.log`) is append-only and trivially
`tail -f`-able. The agent reads:
- Last N lines for a status check.
- Full log between two timestamps for postmortem.
- Grepped for known markers (see watcher).

A small helper `bin/console` wraps:
```
#!/bin/sh
# console tail [N]      tail last N lines
# console grep <pat>    grep entire log
# console clear         truncate log (start a clean run)
# console interact      open the serial socket interactively (for ddb)
```

`console interact` uses `socat - UNIX-CONNECT:run/serial.sock` — full
two-way to the guest console. The agent can type at a ddb prompt from
the shell.

### rsync Helpers

`bin/push`:
```
#!/bin/sh
# Push project tree to guest under /root/h2.
# Excludes VCS, build artifacts, and big binaries.
exec rsync -a --delete \
    --exclude='.git' --exclude='*.o' --exclude='*.ko' \
    --exclude='images/' --exclude='iso/' --exclude='logs/' \
    --exclude='run/' \
    "$(dirname "$0")/.."/ h2dev:/root/h2/
```

`bin/pull`:
```
#!/bin/sh
# Pull crash dumps + test artifacts from guest.
ROOT="$(dirname "$0")/.."
rsync -a h2dev:/var/crash/   "$ROOT/logs/dumps/"     2>/dev/null
rsync -a h2dev:/root/h2-out/ "$ROOT/logs/artifacts/" 2>/dev/null
exit 0
```

Workflow: edit on host → `bin/push` → build/test via SSH → `bin/pull`
for any results. Push is idempotent and fast (diff-only). The
watcher also runs `bin/pull` automatically after every detected
panic-recovery so dumps land on host without manual action.

### Watcher / Health Monitor

A background process `bin/watcher`:
- Every 10 s: `ssh -o ConnectTimeout=3 h2dev true`.
- If 3 consecutive SSH failures:
  - Read last 200 lines of `console.log`.
  - Grep for `panic:`, `Fatal trap`, `KDB:`, `Debugger`, `db>`,
    `Uptime:`, `Rebooting`.
  - Classify state:
    - Panic + auto-reboot in progress → wait 30 s and re-check SSH.
    - Panic + ddb prompt → leave it; raise a signal for the agent.
    - No panic, no progress → lockup. Snapshot console + memory.
      `qmp system_reset`. Mark event in `logs/panics/<timestamp>/`.
  - Write a JSON event file the agent can poll/read.
- Watcher does not interpret bugs — it just classifies and captures.

### Test Harness

`bin/run-test <script>`:
1. `qmp quit` (in case a VM is already up; ignore error).
2. `rm overlay-system.qcow2; qemu-img create ... -b base ...`
3. Regenerate RAID disks via `fresh_disks.sh`.
4. `launch-dfly.sh` (now headless, daemonized).
5. Wait for SSH (poll with 60 s timeout).
6. `bin/push` to sync the working tree to `/root/h2`.
7. `ssh h2dev "cd /root/h2 && sh tests/v4/$script"`.
8. Watcher runs in background throughout.
9. On test exit (any cause): `bin/pull` for dumps + artifacts; collect
   console.log; write a summary to `logs/runs/<timestamp>/`.
10. Return test exit code.

### Build Workflow

Host edits in this repo → `bin/push` → build is one SSH command:
```
bin/push && ssh h2dev "cd /root/h2 && sh build.sh"
```
where `build.sh` runs `make` against the kernel build tree. Artifacts
(kernel, modules) stay in the guest until `bin/pull` brings them back,
or until the next install step.

Install:
```
ssh h2dev "cd /root/h2 && sh install.sh && reboot"
```
Reboot is automatic; SSH reconnects after watcher confirms it's back.

## ISO Acquisition and Install

DragonFlyBSD 6.4.2-RELEASE ISO is at the project's mirrors; URL pattern:
`https://mirror-master.dragonflybsd.org/iso-images/dfly-x86_64-6.4.2_REL.iso.bz2`.
(Verify exact URL at install time; mirror layout occasionally shifts.)

Install procedure, executed once to produce `base-dfly-6.4.2.qcow2`:

1. Download + verify ISO.
2. Create blank `base.qcow2` (20 G).
3. Boot QEMU with `-cdrom iso/dfly-6.4.2-RELEASE.iso -boot d` and the
   harness's serial config (no graphics).
4. The DragonFlyBSD installer runs on serial. Drive it via
   `socat - UNIX-CONNECT:run/serial.sock` — either by hand the first
   time, or via an `expect` script for repeatability.
5. Inside the installer:
   - Partition: single UFS root + swap (use swap as dumpdev).
   - Install base + kernel + src + ports tree.
   - Set hostname `h2dev`.
   - Configure DHCP on `vtnet0`.
   - Enable sshd; install host's pubkey to `/root/.ssh/authorized_keys`.
   - Write the loader.conf, ttys, rc.conf, sysctl.conf snippets from
     §"Guest Configuration."
   - Install rsync (in base or via pkg).
6. Shutdown.
7. Promote the image: `mv working.qcow2 base-dfly-6.4.2.qcow2`. From now
   on, this file is immutable. Overlays are created against it for
   every run.

This is a 30-minute one-time job. After that, every test starts from
the snapshot in ~5 s.

## Failure-Mode Coverage

| Failure | Detected by | Recovery |
|---|---|---|
| Kernel panic, auto-reboot enabled | `console.log` grep for `panic:` + watcher confirms SSH back | Test marked failed; full console + dump captured |
| Kernel panic, ddb prompt | `console.log` grep for `db>` | Watcher signals agent; agent drives ddb via `console interact` |
| Hard lockup (no SSH, no console output) | Watcher: 3× SSH timeout + console idle | `qmp system_reset` after 30 s; test marked failed |
| Userland test hang | Test script timeout (built into `run-test`) | `pkill` via SSH; if SSH dead, treat as lockup |
| QEMU itself crashes | `pidfile` shows process gone | Mark test infrastructure failure; rerun |
| Disk image corruption | qemu-img check fails | Regenerate from base |
| rsync push/pull fails | Non-zero exit from `bin/push` / `bin/pull` | Retry; if SSH dead, treat as lockup |

The agent never needs to ask the human about a hung VM. Either it
recovers automatically or it captures enough state to debug postmortem.

## What I Read

For the next development session, I have these on the host without
any human intervention:
- `logs/console.log` — full kernel output every boot.
- `logs/runs/<ts>/result.json` — pass/fail + reason.
- `logs/dumps/*.{0,info}` — crash dumps pulled via rsync, ready for
  `kgdb` postmortem.
- `logs/panics/<ts>/` — captured pre-reset state on lockup.
- `logs/artifacts/` — test outputs pulled from guest.

`ssh h2dev` works whenever the VM is alive. `qmp query-status` always
answers when QEMU is running. There is no state the human needs to
relay.

## Open Questions

1. **Crash dump on swap**: confirm DragonFlyBSD's `dumpdev=AUTO` works
   the same as FreeBSD's. If not, configure a dedicated dump partition.
2. **expect-script vs. manual install**: first install will be manual
   for speed (~30 min). Decide later whether to automate for clean-room
   reproducibility.
3. **Per-test ephemeral overlays of base**: do we want overlay
   chains (base ← post-build ← per-test) or always reset to base and
   re-`install.sh`? The chain saves ~1 min per test but adds complexity.
   Default: always reset, optimize later if test count justifies it.
4. **rsync in base install**: stock DragonFlyBSD base may or may not
   ship rsync. If not, install from pkg during one-time setup, then
   bake into the base image.

## First Build Order

If this design is accepted:

1. Build host-side directory layout (`run/`, `logs/`, `images/`, `iso/`).
2. Generate host SSH key for the harness.
3. Write `bin/qmp`, `bin/console`, `bin/push`, `bin/pull`, `bin/watcher`,
   `bin/run-test`.
4. Rewrite `launch-dfly.sh` per §"QEMU Invocation."
5. Download DragonFlyBSD 6.4.2 ISO; verify checksum.
6. Run one-time interactive install to produce `base-dfly-6.4.2.qcow2`
   (includes installing rsync + writing host pubkey).
7. Verify: cold boot from base + overlay, SSH in, `bin/push` succeeds,
   build a trivial kernel module, force a panic via
   `sysctl debug.kdb.panic=1`, confirm watcher auto-reboots the guest
   and `bin/pull` retrieves the dump — all without human intervention.
8. Lock the base image.

After step 7 passes, the harness is ready and Phase 0/1 of `newplan.md`
can proceed without screenshots or manual VM interrupts.
