# HAMMER2 RAID6 Development Workflow

## VM Setup

### Launch the VM

```bash
# 4-disk physical test (default)
NDISKS=4 ./launch-dfly.sh

# 6-disk physical test
NDISKS=6 ./launch-dfly.sh
```

QEMU attaches `dfly-raid{0..N-1}.qcow2` (4 GB each) as virtio-blk devices.
Inside DragonFlyBSD these appear as `/dev/vbd0` … `/dev/vbd{N-1}`.

### VM access

```bash
./dfly-exec.sh <command>        # run a command on the VM
./mount-dfly.sh                 # sshfs-mount VM root at mnt/dfly/
ssh root@192.168.25.66          # direct SSH
```

---

## Deploy Cycle

Run after any source change:

```bash
./deploy.sh           # sync sources + build + install + sync tests (all)
./deploy.sh sync      # scp src/sys/local_*.{c,h} and src/sbin/ to VM only
./deploy.sh build     # make clean && make on VM (hammer2.ko + newfs_hammer2 + hammer2)
./deploy.sh install   # cp hammer2.ko /boot/kernel/ + install binaries
./deploy.sh tests     # tar-pipe tests/ -> /root/hammer2-tests/ and src/diag/ -> /root/h2diag/
```

### File mapping (local → VM)

| Local path | VM path |
|---|---|
| `src/sys/local_hammer2*.{c,h}` | `/usr/src/sys/vfs/hammer2/hammer2*.{c,h}` |
| `src/sys/local_vn.c` | `/usr/src/sys/dev/disk/vn/vn.c` |
| `src/sbin/local_mkfs_hammer2.c` | `/usr/src/sbin/newfs_hammer2/mkfs_hammer2.c` |
| `src/sbin/local_cmd_debug.c` | `/usr/src/sbin/hammer2/cmd_debug.c` |
| `src/sbin/local_hammer2_userspace.h` | `/usr/src/sbin/hammer2/hammer2_userspace.h` |
| `tests/` | `/root/hammer2-tests/` |
| `src/diag/` | `/root/h2diag/` |

### Reboot after install

`hammer2.ko` cannot be unloaded while the root filesystem uses it.
After `./deploy.sh install`, reboot to activate:

```bash
./dfly-exec.sh reboot
```

---

## Running Tests

Tests live at `/root/hammer2-tests/` on the VM.

### v4 RAIDZ2-native tests (`tests/raidz2native/`)

```bash
# Default: DISK_MODE=vbd (physical /dev/vbd*), NDISKS=4
./dfly-exec.sh "cd /root/hammer2-tests && sh run_all.sh"

# Specific groups only
./dfly-exec.sh "cd /root/hammer2-tests && sh run_all.sh A B C"

# 6-disk run
./dfly-exec.sh "NDISKS=6 sh /root/hammer2-tests/run_all.sh"

# Fallback: in-memory swap-backed vn devices
./dfly-exec.sh "DISK_MODE=vn NDISKS=6 sh /root/hammer2-tests/run_all.sh"
```

Test groups:

| Group | Description |
|---|---|
| A | Basic read/write, COW invariant, bref encoding |
| B | Single disk failure for each disk position + degraded write |
| C | All C(NDISKS,2) dual-disk failure pairs |
| D | Resilver: basic, sequential, write-during-resilver |
| F | COW slot uniqueness + h2stripe_check parity |
| G | Auto-fail, degraded remount, fail-state persistence |
| I | Unclean unmount (healthy and degraded) |

### v3 md-RAID tests (`tests/mdraid/`)

```bash
./dfly-exec.sh "cd /root/hammer2-tests/mdraid && sh run_all_tests.sh"
```

---

## Disk Mode Reference

| Variable | Values | Default |
|---|---|---|
| `DISK_MODE` | `vbd` (physical QEMU), `vn` (swap-backed) | `vbd` |
| `NDISKS` | 4–6 | 4 |

When `DISK_MODE=vbd`:
- `setup_fresh` calls `newfs_hammer2` directly on `/dev/vbd*` — no vnconfig
- `detach_disk(idx)` is a no-op (disk stays physically present, only software-failed)
- `fresh_disk(idx)` zeros the first 64 MB of the vbd to clear HAMMER2 zone headers

When `DISK_MODE=vn`:
- `setup_fresh` calls `vnconfig -S 1073741824 vn$i` before `newfs_hammer2`
- `detach_disk(idx)` calls `vnconfig -u vn$idx`
- `fresh_disk(idx)` unconfigures and re-configures with a fresh swap backing
