# HAMMER2 RAID6 Development Workflow

## VM Setup

### Launch the VM

Two ways, and which one you want depends on who is driving. The full story is
in `harness/README.md`; the short version:

```bash
# One-shot: a daemonized QEMU, gone when it exits. You are at the keyboard.
./launch-dfly.sh run                 # 4-disk default
NDISKS=6 ./launch-dfly.sh run        # 6-disk

# Supervised: run this ON THE HOST, once. QEMU stays out there with /dev/kvm
# and publishes its control plane into the project, so an agent in the
# claude-box sandbox can reset, cold-boot and snapshot it without the device.
./host-run.sh &
```

Under the sandbox there is no `/dev/kvm`, so a VM booted in there is TCG and
takes 8-12 minutes to reach a login. That is what `host-run.sh` is for, and why
`./vmctl.sh save booted` / `load booted` is worth the one-time cost.

QEMU attaches `images/dfly-raid{0..N-1}.qcow2` (4 GB each, `RAID_SIZE=`
overrides) as virtio-blk devices.  Inside DragonFlyBSD these appear as
`/dev/vbd0` … `/dev/vbd{N-1}`.  The system disk
(`images/overlay-system.qcow2`) is UFS — a kernel-module crash will not
brick the VM.

### VM access

User-mode networking forwards host `127.0.0.1:2322` → guest `:22`.

```bash
./ssh.sh                        # interactive shell
./ssh.sh '<command>'            # run an sh script line on the VM
./ssh.sh --raw -L 8443:localhost:443 -N   # tunnels, stdin, anything raw
./vmctl.sh status               # running? QMP state + does SSH answer
./vmctl.sh wait                 # block until the guest answers SSH
./vmctl.sh log 80               # last 80 lines of serial console
./vmctl.sh console              # attach to the live console (detach: C-])
./vmctl.sh save booted          # snapshot; `load booted` restores in seconds
```

`ssh.sh` resolves host, port and key from `vmenv.sh` — **not** from a `h2dev`
entry in `~/.ssh/config`, which does not exist inside the claude-box sandbox.
See `harness/README.md` under "SSH identity" for where it looks and what to do
when it finds nothing. The `h2dev` alias still works on the host, and `bin/`,
`vmctl.sh` and `../dfly/bin/` all go through `vmenv.sh` now.

Do **not** use IP literals.  The old `192.168.25.66` / `.102` addresses
are from the prior LAN-bridged VM and no longer reach anything.

---

## Deploy Cycle

The guest runs DragonFly master at the overlay's base (48147b0412, the
flynas fork's `arm64-base`), built from `/usr/src` (a git checkout, branch
`master-h2`) with `KERNCONF=H2DEV`: X86_64_GENERIC minus `options HAMMER2`,
so hammer2 is a module.  The 6.4.2 kernel is `/boot/kernel.old`.
A `pre-master` VM snapshot holds the old 6.4.2 guest, `master-base` the
freshly upgraded one.  Rebuilding the world on the guest: `make -j2
NO_ALTCOMPILER=yes buildworld`, and pass `NO_ALTCOMPILER=yes` to
`installworld`/`upgrade` too — without it they try to install the gcc120
that was never built and stop half way (which once left a new sshd without
its `sshd-session`, refusing every connection).

```bash
./deploy.sh fast      # sync + build + reload + tests
                      # — NO reboot; requires no hammer2 fs mounted.
                      # Use this for the inner loop.
./deploy.sh all       # sync + build + install + tests (reboot after)
./deploy.sh sync      # bin/apply-overlay on the guest's /usr/src
                      # + regenerate sys/config/H2DEV
./deploy.sh build     # KMOD hammer2.ko in /usr/src/sys/vfs/hammer2
                      # + newfs_hammer2 + hammer2 userspace tools
./deploy.sh install   # quickkernel + reinstallkernel (loader-compatible
                      # hammer2.ko in /boot/kernel) + install binaries
./deploy.sh reload    # kldunload + kldload the KMOD hammer2.ko
./deploy.sh tests     # tar-pipe tests/ -> /root/hammer2-tests/
                      # + src/diag/ -> /root/h2diag/
```

The `fast` action is the inner-loop workhorse: source edit → tested
behaviour in ~20 seconds.  `install` needs a prior
`make -j2 KERNCONF=H2DEV buildkernel` on the guest to populate
`/usr/obj/usr/src/sys/H2DEV/`; the KMOD .ko from `build` is rejected by
the loader with `file has no contents`.

Env: `KERNCONF` (default `H2DEV`), `DFLY_HOST` (default: the harness
identity from `vmenv.sh`).

### File mapping (local → VM)

`deploy.sh sync` runs `bin/apply-overlay /usr/src` on the guest, so the
mapping is the same as for a local tree: `src/sys/local_*` →
`sys/vfs/hammer2/`, `src/sbin/local_{mkfs,newfs}_*` →
`sbin/newfs_hammer2/`, every other `src/sbin/local_*` → `sbin/hammer2/`
(prefix stripped).  It also adds `vfs/hammer2/hammer2_raid6.c optional
hammer2` to `sys/conf/files` and `SRCS+= cmd_raid.c` to the hammer2
Makefile if missing.  `tests/` → `/root/hammer2-tests/`, `src/diag/` →
`/root/h2diag/` (`deploy.sh tests`).

### Reboot after install

`hammer2.ko` cannot be unloaded while a hammer2 filesystem is mounted.
After `./deploy.sh install`, reboot (or `./deploy.sh reload`):

```bash
./vmctl.sh reboot     # graceful, waits for SSH
```

`make clean && make` is required when struct layouts change — old
forwarder objects link silently against stale layouts otherwise.

---

## Running Tests

Tests live at `/root/hammer2-tests/` on the VM after `./deploy.sh tests`.

### v3 RAIDZ2-native tests (`tests/v3/`)

```bash
ssh h2dev 'cd /root/hammer2-tests/v3 && sh run_all.sh'

# Specific groups only
ssh h2dev 'cd /root/hammer2-tests/v3 && sh run_all.sh A B C'

# 6-disk run
ssh h2dev 'cd /root/hammer2-tests/v3 && NDISKS=6 sh run_all.sh'
```

Test groups:

| Group | Description |
|---|---|
| A | Basic read/write, COW invariant, bref encoding |
| B | Single disk failure for each disk position + degraded write |
| C | All C(NDISKS,2) dual-disk failure pairs |
| D | Resilver: basic, sequential, write-during-resilver |
| F | COW slot uniqueness + parity check |
| G | Auto-fail, degraded remount, fail-state persistence |
| I | Unclean unmount (healthy and degraded) |

### Perf harness (`tests/perf/`)

See `tests/perf/README.md`.  Requires fio installed on the VM.

```bash
ssh h2dev 'cd /root/hammer2-tests/perf && sh run_perf.sh'
```

Substrate is virtio-blk only.  The vn-backed substrate was removed in
Group K (commit `37d3808`); any reference to `DISK_MODE=vn`,
`vnconfig`, or `/dev/vn*` is from an older era.

---

## Gotchas

### "Ambiguous output redirect" from tcsh

`./ssh.sh '<cmd>'` sidesteps this entirely — it pipes the script to `sh` in
the guest, so ordinary sh redirection works. The rest of this section applies
to a bare `ssh`, including `./ssh.sh --raw`.

Root's shell on DragonFly is **tcsh**, which is csh-family.  Bash-style
redirection in `ssh h2dev '<cmd>'` is silently invalid:

```bash
ssh h2dev 'ls /a 2>&1 | head'        # FAILS: Ambiguous output redirect
ssh h2dev 'cmd > /tmp/out 2>&1'      # FAILS same way
```

tcsh wants `>&` for combined redirect; mixing `>` and `2>&1` breaks it.
Always wrap remote commands with `sh -c` so bash syntax is parsed by
a bash-family shell on the VM:

```bash
ssh h2dev sh -c 'ls /a 2>&1 | head'                  # OK
ssh h2dev "sh -c 'cd /usr/src && make 2>&1 | tail'"  # OK (nested quoting)
```

Or do the redirection on the *host* side, where it's bash:

```bash
ssh h2dev ls /a 2>&1 | head                          # OK — host bash sees 2>&1
```

Symptom: `ssh` exits non-zero with `Ambiguous output redirect.` on stderr
and the command never ran.  The error comes from tcsh on the VM, not ssh
itself.

### dmesg checks after a test run

```bash
ssh h2dev sh -c 'dmesg | grep "CHECK FAIL"'       # data corruption
ssh h2dev sh -c 'dmesg | grep -i "sync error"'    # flush I/O failures
ssh h2dev sh -c 'dmesg | grep -i panic'           # kernel panics
ssh h2dev sh -c 'dmesg -c > /dev/null'            # clear before test
```

All wrapped in `sh -c` per the redirect rule above.

### QEMU-level recovery (when ssh is unreachable)

If hammer2 deadlocks badly enough that ssh hangs:

```bash
./vmctl.sh reset                 # hard reset — the fast reboot
./vmctl.sh nmi                   # break the wedged kernel into ddb instead
./vmctl.sh console               # ...and drive the ddb prompt from here
./vmctl.sh coldboot              # quit; host-run.sh boots a fresh QEMU
./vmctl.sh quit                  # graceful via QMP socket
pkill -9 qemu-system-x86_64      # nuclear option
./launch-dfly.sh run             # restart (one-shot mode only)
```

System root is UFS, so a hard kill triggers fsck on next boot — watch
via `bin/console follow`.

### Loading hammer2 inside test scripts

Test scripts must self-load the module; UFS root doesn't pull it in:

```sh
kldstat -q -m hammer2 || kldload hammer2
```

This is already in every `tests/v3/test_*.sh` — copy the pattern for
new scripts.
