# Base Image Install Runbook

One-time procedure to produce `images/base-dfly-6.4.2.qcow2`. Run once;
afterwards every test boot is a fresh thin overlay from the locked base.

## Prerequisites

- ISO at `iso/dfly-x86_64-6.4.2_REL.iso` (downloaded + MD5 verified).
- `socat` installed on the host (`sudo apt install socat` if missing).
- The harness scripts in `bin/` and `launch-dfly.sh` in place.

## Step 1 — Boot the installer

In one terminal:

```
./launch-dfly.sh install
```

The script creates `images/overlay-system.qcow2` (20 G blank) and starts
QEMU daemonized. Console output flows to `logs/console.log` and the
serial socket at `run/serial.sock`.

## Step 2 — Drive the installer

In a second terminal:

```
./bin/console interact
```

This attaches to the serial socket. You'll see the DragonFlyBSD loader,
then the installer menus. Escape with `Ctrl-]` when done. From here:

1. At the loader prompt, accept the default boot. The installer launches.
2. **Installer language**: leave default.
3. **Country**: as appropriate.
4. **Keymap**: as appropriate.
5. **Hostname**: enter `h2dev`.
6. **Disk layout**:
   - Select `vtbd0` (the system disk; 20 G).
   - Partition: choose a simple **UFS** layout. Use auto-partition if
     offered. Ensure there is a **swap partition large enough to hold
     a kernel core dump** — at least equal to RAM, so ≥ 4 G.
   - The installer should set the swap as dumpdev automatically; if it
     offers a choice, set it.
7. **Distribution sets**: install the full base + kernel + sources +
   ports tree (we will be building kernels).
8. **Network**: configure DHCP on `vtnet0`. (The user-mode SLIRP net
   provides 10.0.2.x; SSH forwarding is configured by `launch-dfly.sh`.)
9. **Root password**: pick any; we will use SSH key auth.
10. **Add user**: skip or create one — does not matter; tests run as root.
11. **Set timezone**: as appropriate.
12. **Enable sshd**: YES.
13. Reboot when the installer offers.

The reboot will boot the freshly-installed system. The loader and
console should both come up on serial.

## Step 3 — Apply harness configuration

After first login (root, password from step 9), copy the harness
configuration into the guest. Easiest path: bring up the network, fetch
the apply.sh bundle from the host over SSH (host running an ad-hoc
sshd? skip — we'll paste manually).

**Simplest path**: paste the apply.sh contents and adjacent files into
the guest via `console interact`. Use `cat > /tmp/foo.sh <<'EOF' ... EOF`.

Or, if `pkg` is reachable and you've configured the network, install
`rsync` and pull from the host's IP (the host shows as `10.0.2.2`
inside the guest):

```
pkg install -y rsync
mkdir -p /tmp/cfg
# From host, the harness directory needs to be reachable — easiest is
# to scp from the guest back to the host's SSH:
# scp -P <host_ssh_port> bz@10.0.2.2:/home/bz/code/hammer2-raid6/harness/guest-config/ /tmp/cfg/
# (Requires sshd on host. Otherwise paste-via-console is faster.)
cd /tmp/cfg
sh apply.sh
```

The `apply.sh` script:

- Appends serial-console settings to `/boot/loader.conf`.
- Configures `ttyu0` in `/etc/ttys` for serial getty.
- Sets hostname, sshd, dumpdev, DHCP in `/etc/rc.conf`.
- Disables `debugger_on_panic`, sets 5-second panic reboot wait in
  `/etc/sysctl.conf`.
- Installs the harness SSH pubkey to `/root/.ssh/authorized_keys`.
- Ensures `rsync` is present.

## Step 4 — Shutdown and promote

Inside the guest:

```
shutdown -p now
```

QEMU exits. Back on host:

```
mv images/overlay-system.qcow2 images/base-dfly-6.4.2.qcow2
chmod 0444 images/base-dfly-6.4.2.qcow2
```

The base image is now locked read-only. From here on, every
`./launch-dfly.sh run` creates a fresh overlay from this base in ~1 s.

## Step 5 — Verify

```
./launch-dfly.sh run            # auto-creates overlay
# wait ~30 s for boot
ssh h2dev uname -a              # should print DragonFly v6.4.2-RELEASE
./bin/push                      # verify push works
./bin/qmp query-status          # verify QMP
./bin/watcher start             # arm the watcher
ssh h2dev sysctl debug.kdb.panic=1   # force a panic
# Console log should show panic, kernel reboot, savecore, login back.
# bin/pull should retrieve the dump.
./bin/watcher stop
./bin/qmp quit
```

If the panic→reboot→savecore cycle completes without the human
touching anything, the harness is working as designed.
