# Skills: DragonFlyBSD Development Workflow

A reference for the tools and techniques used to develop, build, and test
the HAMMER2 RAID6 kernel module on a remote DragonFlyBSD VM.

---

## VM Access

Full harness reference: `harness/README.md`.

```sh
# Run a command on the VM (as root, forced through sh past the tcsh login)
./ssh.sh <command>          # ./dfly-exec.sh is a shim for the same thing

# Examples:
./ssh.sh 'uname -a'
./ssh.sh 'kldstat | grep hammer2'
./ssh.sh 'cd /usr/src/sys/vfs/hammer2 && make 2>&1 | tail -20'

# Redirection works: the command is piped to sh in the guest, so tcsh never
# sees it. Only `./ssh.sh --raw ...` (tunnels, stdin) hits tcsh directly.
./ssh.sh                    # interactive shell
```

Host, port (2322) and SSH key come from `vmenv.sh`, not from `~/.ssh/config` —
there is no `~/.ssh` inside the claude-box sandbox. If it says "no SSH
identity", see `harness/README.md`.

## SSHFS Mount (Browse/Edit Files Directly)

```sh
# Mount DragonFlyBSD root filesystem locally
./mount-dfly.sh
# Files appear at: mnt/dfly/

# Examples:
# Read a file:    mnt/dfly/usr/src/sys/vfs/hammer2/hammer2_io.c
# Edit directly:  vim mnt/dfly/usr/src/sys/vfs/hammer2/hammer2_io.c
# Copy to VM:     cp local.c mnt/dfly/usr/src/sys/vfs/hammer2/hammer2_local.c
```

## Copying Files To/From VM

```sh
# `vm:` stands for the guest — ./scp.sh fills in host, port and key.
# Copy a file TO the VM
./scp.sh local_file.c vm:/usr/src/sys/vfs/hammer2/

# Copy a file FROM the VM
./scp.sh vm:/usr/src/sys/vfs/hammer2/hammer2_io.c ./

# Copy a patch FROM the VM
./scp.sh vm:/tmp/hammer2_raid6_full.patch ./hammer2_raid6.patch

# Whole trees: bin/push (host -> /root/h2) and bin/pull (dumps + artifacts).
```

## Building the Kernel Module

The guest runs DragonFly master (the overlay's base, 48147b0412) built from
/usr/src with `KERNCONF=H2DEV` — X86_64_GENERIC minus `options HAMMER2`, so
hammer2 is a loadable module. The 6.4.2 kernel is kept as /boot/kernel.old.

```sh
# Inner loop: KMOD build + kldunload/kldload (no reboot, nothing hammer2 mounted)
./deploy.sh fast

# Loader-compatible module: quickkernel + reinstallkernel, then reboot or reload
./deploy.sh install

# KMOD build alone (kldload-able, but the loader rejects it at boot)
./dfly-exec.sh 'cd /usr/src/sys/vfs/hammer2 && make 2>&1 | tail -30'
```

## Building Userspace Tools

```sh
# newfs_hammer2
./dfly-exec.sh 'cd /usr/src/sbin/newfs_hammer2 && make clean && make 2>&1 | tail -10'

# hammer2 utility
./dfly-exec.sh 'cd /usr/src/sbin/hammer2 && make clean && make 2>&1 | tail -10'

# Install (use install(1), NOT cp — cp fails "Text file busy" on running binary)
./dfly-exec.sh 'install -m 755 /usr/src/sbin/hammer2/hammer2 /sbin/hammer2'
./dfly-exec.sh 'install -m 755 /usr/src/sbin/newfs_hammer2/newfs_hammer2 /sbin/newfs_hammer2'
```

## Rebooting the VM

```sh
# Reboot (required after installing a new hammer2.ko — root fs uses it)
# and block until it is answering SSH again:
./vmctl.sh reboot

# Hard reset instead, when the guest is too wedged to reboot itself:
./vmctl.sh reset
```

## Running the VM (QEMU)

```sh
# One-shot: daemonized QEMU, headless, gone when it exits
./launch-dfly.sh run

# Supervised (run this ON THE HOST): QEMU stays out there with /dev/kvm and
# publishes its control plane into run/ + logs/, so a sandboxed session can
# reset, cold-boot and snapshot it. Without this the box boots under TCG:
# 8-12 minutes to a login, every time.
./host-run.sh &

./vmctl.sh wait             # block until the guest answers SSH
./vmctl.sh save booted      # ...then stop paying for boots
./vmctl.sh load booted
./vmctl.sh status | log 80 | console | reset | coldboot | nmi | quit
```

The VM is headless with slirp networking — no bridge, no window, no IP of its
own. Anything mentioning 192.168.25.x is from the retired bridged setup.

## Virtual Disk Management (vnconfig)

```sh
# Create a 1GB disk image
./dfly-exec.sh 'truncate -s 1073741824 /var/tmp/disk0.img'

# Attach disk image to vn device
./dfly-exec.sh 'vnconfig vn0 /var/tmp/disk0.img'

# Detach vn device
./dfly-exec.sh 'vnconfig -u vn0'

# List configured vn devices
./dfly-exec.sh 'vnconfig -l'

# NOTE: Disk images live in /var/tmp/ (not /tmp/ — tmpfs is only 486MB)
```

## Formatting and Mounting RAID6

```sh
# Format 4 vn devices as RAID6 with label TEST
./dfly-exec.sh 'newfs_hammer2 -R 6 -L TEST /dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3'

# Mount the RAID6 filesystem
./dfly-exec.sh 'mkdir -p /mnt/test && mount -t hammer2 /dev/vn0:/dev/vn1:/dev/vn2:/dev/vn3@TEST /mnt/test'

# Check RAID status
./dfly-exec.sh 'hammer2 raid status /dev/vn0'

# Unmount
./dfly-exec.sh 'umount /mnt/test'
```

## Generating a Patch

The patch is the overlay in src/ applied to DragonFly master at the flynas
fork's `arm64-base` tag (48147b0412), diffed on the host:

```sh
S=$(mktemp -d)
git -C ../dragonfly worktree add --detach $S/base arm64-base
bin/apply-overlay $S/base
git -C $S/base add -N . && git -C $S/base diff > hammer2_raid6.patch
git -C ../dragonfly worktree remove --force $S/base
```

## Applying a Patch (On VM)

/usr/src on the guest is a git checkout of 48147b0412 (branch master-h2),
so prefer `./deploy.sh sync`; the patch is for other trees at that base:

```sh
./scp.sh hammer2_raid6.patch vm:/tmp/hammer2_raid6.patch
./dfly-exec.sh 'cd /usr/src && git apply --check /tmp/hammer2_raid6.patch && git apply /tmp/hammer2_raid6.patch'
```

## Checking dmesg

```sh
# View kernel messages (useful after mount, reboot, or panic)
./dfly-exec.sh 'dmesg | tail -40'

# Grep for HAMMER2 messages
./dfly-exec.sh 'dmesg | grep -i hammer2'
```

## Running Filesystem Tests

```sh
# Run Test B (single disk failure)
./dfly-exec.sh 'sh /var/tmp/test_b.sh'

# Run Test D (online resilver + remount integrity)
./dfly-exec.sh 'sh /var/tmp/test_d.sh'

# Run parity checker
./dfly-exec.sh '/var/tmp/h2parity_fix -n /var/tmp/disk0.img /var/tmp/disk1.img /var/tmp/disk2.img /var/tmp/disk3.img'
```
