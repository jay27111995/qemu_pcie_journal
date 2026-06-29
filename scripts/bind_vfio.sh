#!/bin/bash
# Bind edu-pci device to vfio-pci

DEVICE="0000:00:03.0"
VENDOR="1234"
DEVID="ed01"

echo "Unbinding $DEVICE..."
echo $DEVICE > /sys/bus/pci/devices/$DEVICE/driver/unbind 2>/dev/null

echo "Binding to vfio-pci..."
echo "$VENDOR $DEVID" > /sys/bus/pci/drivers/vfio-pci/new_id 2>/dev/null

echo "Done. VFIO groups:"
ls /dev/vfio/
