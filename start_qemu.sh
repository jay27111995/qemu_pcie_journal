#!/bin/bash
# Start QEMU with Linux for PCI learning

DIR="$(cd "$(dirname "$0")" && pwd)"
KERNEL=$DIR/linux/arch/x86/boot/bzImage
QEMU=$DIR/qemu/build/qemu-system-x86_64
DEBIAN=$DIR/debian-12-nocloud-amd64.qcow2

# Enable gcc-toolset for QEMU dependencies
source /opt/rh/gcc-toolset-12/enable
export LD_LIBRARY_PATH=$HOME/local/lib64:$LD_LIBRARY_PATH

$QEMU \
  -m 512M \
  -machine q35,accel=kvm,kernel-irqchip=split \
  -kernel $KERNEL \
  -hda $DEBIAN \
  -append "root=/dev/sda1 console=ttyS0 intel_iommu=on" \
  -nographic \
  -device intel-iommu,intremap=on,caching-mode=on,eim=on \
  -device edu-nvme,id=nvme0 \
  -device edu-pci,id=pf0 \
  -device edu-pci,id=pf1 \
  -device qemu-xhci,id=xhci \
  -device usb-edu-serial,bus=xhci.0 \
  -virtfs local,path=$DIR/tests,mount_tag=tests,security_model=none \
  -virtfs local,path=$DIR/utils,mount_tag=utils,security_model=none \
  -virtfs local,path=$DIR/drivers,mount_tag=drivers,security_model=none \
  -virtfs local,path=$DIR/examples,mount_tag=examples,security_model=none

# Login: root (no password)
# Exit with Ctrl-A X
