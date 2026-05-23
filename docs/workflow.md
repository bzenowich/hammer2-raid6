# HAMMER2 RAID6 Development Workflow

## VM Setup

### Launch the VM

```bash
# 4-disk default
./launch-dfly.sh

# 6-disk
NDISKS=6 ./launch-dfly.sh
```

QEMU attaches `images/dfly-raid{0..N-1}.qcow2` (4 GB each, `RAID_SIZE=`
overrides) as virtio-blk devices.  Inside DragonFlyBSD these appear as
`/dev/vbd0` … `/dev/vbd{N-1}`.  The system disk
(`images/overlay-system.qcow2`) is UFS — a kernel-module crash will not
brick the VM.

### VM access

User-mode networking forwards host `127.0.0.1:2322` → guest `:22`.  An
SSH alias `h2dev` is preconfigured.

```bash
ssh h2dev <command>             # run a command on the VM
scp src.c h2dev:/path           # copy file
bin/console follow              # attach to serial console (kprintf log)
bin/qmp query-status            # QEMU monitor command
```

Do **not** use IP literals.  The old `192.168.25.66` / `.102` addresses
are from the prior LAN-bridged VM and no longer reach anything.

---

## Deploy Cycle

```bash
./deploy.sh fast      # sync + build + install + reload + tests
                      # — NO reboot; requires no hammer2 fs mounted.
                      # Use this for the inner loop.
./deploy.sh all       # sync + build + install + tests (reboot after)
./deploy.sh sync      # scp local_* to /usr/src/sys/vfs/hammer2/...
                      # + idempotently patch /usr/src/sys/conf/files
./deploy.sh build     # incrementally rebuild hammer2.ko in the
                      # /usr/obj kernel-tree obj dir (loader-compatible)
                      # + newfs_hammer2 + hammer2 userspace tools
./deploy.sh install   # cp hammer2.ko /boot/kernel/ + install binaries
./deploy.sh reload    # kldunload + kldload hammer2 (no reboot)
./deploy.sh tests     # tar-pipe tests/ -> /root/hammer2-tests/
                      # + src/diag/ -> /root/h2diag/
```

The `fast` action is the inner-loop workhorse: source edit → tested
behaviour in ~20 seconds (vs ~20 min for a full kernel rebuild + reboot).
The first build still needs a one-time `make nativekernel KERNCONF=H2DEV`
on the VM to populate `/usr/obj/usr/src/sys/H2DEV/` — without it,
`deploy.sh build` falls back to the KMOD .ko which the DragonFly loader
rejects with `file has no contents`.

Env: `DFLY_HOST` (default `h2dev`).

### File mapping (local → VM)

| Local path | VM path |
|---|---|
| `src/sys/local_hammer2*.{c,h}` | `/usr/src/sys/vfs/hammer2/hammer2*.{c,h}` |
| `src/sys/local_Makefile` | `/usr/src/sys/vfs/hammer2/Makefile` |
| `src/sbin/local_mkfs_hammer2.{c,h}` | `/usr/src/sbin/newfs_hammer2/mkfs_hammer2.{c,h}` |
| `src/sbin/local_newfs_hammer2.c` | `/usr/src/sbin/newfs_hammer2/newfs_hammer2.c` |
| `src/sbin/local_cmd_debug.c` | `/usr/src/sbin/hammer2/cmd_debug.c` |
| `src/sbin/local_hammer2_userspace.h` | `/usr/src/sbin/hammer2/hammer2_userspace.h` |
| `tests/` | `/root/hammer2-tests/` |
| `src/diag/` | `/root/h2diag/` |

`deploy.sh sync` also appends `vfs/hammer2/hammer2_raid6.c optional
hammer2` to `/usr/src/sys/conf/files` if not already there (idempotent).
Both the kernel Makefile and the static-kernel build path need this
file listed.

### Reboot after install

`hammer2.ko` cannot be unloaded while a hammer2 filesystem is mounted.
After `./deploy.sh install`, reboot:

```bash
ssh h2dev shutdown -r now
until ssh -o ConnectTimeout=3 h2dev 'echo up' 2>/dev/null; do sleep 3; done
```

`make clean && make` is required when struct layouts change — old
forwarder objects link silently against stale layouts otherwise.

---

## Running Tests

Tests live at `/root/hammer2-tests/` on the VM after `./deploy.sh tests`.

### v4 RAIDZ2-native tests (`tests/v4/`)

```bash
ssh h2dev 'cd /root/hammer2-tests/v4 && sh run_all.sh'

# Specific groups only
ssh h2dev 'cd /root/hammer2-tests/v4 && sh run_all.sh A B C'

# 6-disk run
ssh h2dev 'cd /root/hammer2-tests/v4 && NDISKS=6 sh run_all.sh'
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
bin/qmp quit                     # graceful via QMP socket
pkill -9 qemu-system-x86_64      # nuclear option
./launch-dfly.sh                 # restart
```

System root is UFS, so a hard kill triggers fsck on next boot — watch
via `bin/console follow`.

### Loading hammer2 inside test scripts

Test scripts must self-load the module; UFS root doesn't pull it in:

```sh
kldstat -q -m hammer2 || kldload hammer2
```

This is already in every `tests/v4/test_*.sh` — copy the pattern for
new scripts.
