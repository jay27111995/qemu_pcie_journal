#!/bin/bash
# Bind edu-pci device to vfio-pci
# edu-pci devices are at 00:04.0 and 00:05.0 (vendor 1234, device ed01)

set -e

DEVICE="0000:00:04.0"
VENDOR="1234"
DEVID="ed01"

echo "Loading vfio-pci module..."
modprobe vfio-pci 2>/dev/null || true

echo "Unbinding $DEVICE from current driver..."
if [ -e /sys/bus/pci/devices/$DEVICE/driver ]; then
    echo $DEVICE > /sys/bus/pci/devices/$DEVICE/driver/unbind 2>/dev/null || true
fi

echo "Binding to vfio-pci..."
echo "$VENDOR $DEVID" > /sys/bus/pci/drivers/vfio-pci/new_id 2>/dev/null || true

# Give it a moment to settle
sleep 1

echo "Done. VFIO groups:"
ls -la /dev/vfio/

echo ""
echo "PCI devices:"
lspci | grep -i "1234\|ed01" || lspci | head -10
