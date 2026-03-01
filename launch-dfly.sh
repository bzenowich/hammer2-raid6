#!/bin/bash

ISO="dfly-x86_64-6.4.2_REL.iso"
DISK="dfly-disk.qcow2"
ROOT_DISK="dfly-root.qcow2"
DISK_SIZE="20G"
ROOT_DISK_SIZE="8G"
RAM="2G"
CPUS="2"
BRIDGE="br0"
HOST_IF="enp0s25"
TAP_IF="tap0"

# Create disk image if it doesn't exist
if [ ! -f "$DISK" ]; then
    echo "Creating disk image ($DISK_SIZE)..."
    qemu-img create -f qcow2 "$DISK" "$DISK_SIZE"
fi

# Create root disk image if it doesn't exist
if [ ! -f "$ROOT_DISK" ]; then
    echo "Creating UFS root disk image ($ROOT_DISK_SIZE)..."
    qemu-img create -f qcow2 "$ROOT_DISK" "$ROOT_DISK_SIZE"
fi

# Set up bridge networking if not already configured
if ! ip link show "$BRIDGE" &>/dev/null; then
    echo "Setting up bridge $BRIDGE..."
    sudo ip link add name "$BRIDGE" type bridge
    sudo ip link set "$HOST_IF" master "$BRIDGE"
    # Move IP from host interface to bridge
    HOST_IP=$(ip -4 addr show "$HOST_IF" | grep -oP 'inet \K[0-9./]+')
    HOST_GW=$(ip route | grep "default.*$HOST_IF" | awk '{print $3}')
    sudo ip addr flush dev "$HOST_IF"
    sudo ip addr add "$HOST_IP" dev "$BRIDGE"
    sudo ip link set "$BRIDGE" up
    if [ -n "$HOST_GW" ]; then
        sudo ip route add default via "$HOST_GW" dev "$BRIDGE"
    fi
fi

# Create tap interface for QEMU
if ! ip link show "$TAP_IF" &>/dev/null; then
    sudo ip tuntap add dev "$TAP_IF" mode tap user "$(whoami)"
    sudo ip link set "$TAP_IF" master "$BRIDGE"
    sudo ip link set "$TAP_IF" up
fi

exec qemu-system-x86_64 \
    -m "$RAM" \
    -smp "$CPUS" \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -hda "$DISK" \
    -hdb "$ROOT_DISK" \
    -boot c \
    -enable-kvm \
    -cpu host \
    -device VGA,edid=on,xres=1024,yres=768 \
    -display gtk,window-close=off \
    -netdev tap,id=net0,ifname="$TAP_IF",script=no,downscript=no \
    -device virtio-net-pci,netdev=net0
