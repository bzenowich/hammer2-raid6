# Skills: DragonFlyBSD Development Workflow

A reference for the tools and techniques used to develop, build, and test
the HAMMER2 RAID6 kernel module on a remote DragonFlyBSD VM.

---

## VM Access

```sh
# Run a command on the VM (SSHes as root)
./dfly-exec.sh <command>

# Examples:
./dfly-exec.sh 'uname -a'
./dfly-exec.sh 'kldstat | grep hammer2'
./dfly-exec.sh 'cd /usr/src && make'

# IMPORTANT: VM shell is tcsh — use single-quoted sh -c for compound commands
./dfly-exec.sh 'cd /usr/src/sys/vfs/hammer2 && make 2>&1 | tail -20'

# IMPORTANT: Avoid 2>&1 outside of sh -c; tcsh treats it as ambiguous
# BAD:  ./dfly-exec.sh 'make 2>&1'
# GOOD: ./dfly-exec.sh 'make > /tmp/out.txt 2>&1 && cat /tmp/out.txt'
```

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
# Copy a file TO the VM
scp local_file.c root@192.168.25.102:/usr/src/sys/vfs/hammer2/

# Copy a file FROM the VM
scp root@192.168.25.102:/usr/src/sys/vfs/hammer2/hammer2_io.c ./

# Copy a patch FROM the VM
scp root@192.168.25.102:/tmp/hammer2_raid6_full.patch ./hammer2_raid6.patch
```

## Building the Kernel Module

```sh
# Build hammer2.ko (no full kernel recompile needed)
./dfly-exec.sh 'cd /usr/src/sys/vfs/hammer2 && make 2>&1 | tail -30'

# Build with clean
./dfly-exec.sh 'cd /usr/src/sys/vfs/hammer2 && make clean && make 2>&1 | tail -30'

# Install the new module (requires reboot to take effect — root fs uses hammer2)
./dfly-exec.sh 'cp /usr/src/sys/vfs/hammer2/hammer2.ko /boot/kernel/hammer2.ko'
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
./dfly-exec.sh 'reboot'

# Wait ~30 seconds, then check if it's back:
sleep 30 && ./dfly-exec.sh 'uname -a'
```

## Running the VM (QEMU)

```sh
# Start the DragonFlyBSD VM (from this project directory)
./launch-dfly.sh

# The VM gets IP 192.168.25.102 via DHCP on the bridge
# Initial window size: 1024x768 (via OVMF EDID)
```

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

```sh
# On the VM (from /usr/src):
./dfly-exec.sh 'cd /usr/src && git diff > /tmp/raid6_tracked.patch'

# Add new (untracked) files to the patch:
./dfly-exec.sh 'cd /usr/src && git diff --no-index /dev/null sbin/hammer2/cmd_raid.c > /tmp/newfile_cmd_raid.patch'
./dfly-exec.sh 'cd /usr/src && git diff --no-index /dev/null sys/vfs/hammer2/hammer2_raid6.c > /tmp/newfile_raid6c.patch'
./dfly-exec.sh 'cd /usr/src && git diff --no-index /dev/null sys/vfs/hammer2/hammer2_raid6.h > /tmp/newfile_raid6h.patch'

# Combine:
./dfly-exec.sh 'cat /tmp/raid6_tracked.patch /tmp/newfile_cmd_raid.patch /tmp/newfile_raid6c.patch /tmp/newfile_raid6h.patch > /tmp/hammer2_raid6_full.patch'

# Copy to local project:
scp root@192.168.25.102:/tmp/hammer2_raid6_full.patch ./hammer2_raid6.patch
```

## Applying a Patch (On VM)

```sh
# Test patch before applying:
./dfly-exec.sh 'cd /usr/src && patch --check -p1 < /tmp/hammer2_raid6.patch'

# Apply:
./dfly-exec.sh 'cd /usr/src && patch -p1 < /tmp/hammer2_raid6.patch'
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
