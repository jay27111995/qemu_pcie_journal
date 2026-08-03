#!/bin/bash
# Mount all shared folders from host

mkdir -p /mnt/tests /mnt/utils /mnt/drivers /mnt/examples

mount -t 9p -o trans=virtio tests /mnt/tests
mount -t 9p -o trans=virtio utils /mnt/utils
mount -t 9p -o trans=virtio drivers /mnt/drivers
mount -t 9p -o trans=virtio examples /mnt/examples

echo "Mounted:"
echo "  /mnt/tests    - VFIO, interrupt, NVMe tests"
echo "  /mnt/utils    - bind_vfio.sh, dma.py"
echo "  /mnt/drivers  - kernel modules"
echo "  /mnt/examples - C/C++ examples"
