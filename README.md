# QEMU PCIe Learning Journal

## Overview

Scripts and examples for learning PCIe, VFIO, DMA, and Linux kernel driver development using QEMU.

## Quick Start

```bash
./start_qemu.sh  # Start QEMU with edu-pci devices

# Inside guest:
bash -c "$(cat /proc/cmdline | grep -o 'mount_all')" || {
  mkdir -p /mnt/tests /mnt/utils /mnt/drivers /mnt/examples
  mount -t 9p -o trans=virtio tests /mnt/tests
  mount -t 9p -o trans=virtio utils /mnt/utils
  mount -t 9p -o trans=virtio drivers /mnt/drivers
  mount -t 9p -o trans=virtio examples /mnt/examples
}

# Or simply:
bash /mnt/utils/mount_all.sh   # After mounting utils first
```

## Directory Structure

```
qemu_learn/
├── drivers/           # Kernel modules
│   ├── edu_pci.c      # Linux kernel driver for edu-pci
│   └── Makefile
│
├── tests/
│   ├── vfio/          # VFIO userspace tests
│   │   ├── test_vfio.py        # Basic BAR access
│   │   ├── test_vfio_dma.py    # DMA mapping (Python)
│   │   ├── test_edu_dma.cpp    # DMA + MSI-X (C++)
│   │   ├── test_bad_dma.py     # IOMMU fault handling
│   │   ├── test_ats.cpp        # Address Translation Services
│   │   └── test_p2p.sh         # P2P device-to-device DMA
│   │
│   ├── interrupts/    # Interrupt tests
│   │   ├── test_intx.py        # Legacy INTx
│   │   ├── test_msix.py        # MSI-X via VFIO
│   │   ├── test_msix_direct.py # Direct MSI-X programming
│   │   └── test_msix_devmem.py # MSI-X via /dev/mem
│   │
│   └── nvme/          # NVMe tests
│       ├── test_nvme.py        # NVMe queues (Python)
│       └── test_nvme.cpp       # Full NVMe test (C++)
│
├── utils/             # Utility scripts
│   ├── bind_vfio.sh   # Bind device to vfio-pci
│   ├── setup_mount.sh # Mount single virtfs
│   ├── mount_all.sh   # Mount all virtfs shares
│   └── dma.py         # DMA helper functions
│
├── examples/          # C/C++ learning examples
│   ├── eventfd_demo.c # Linux eventfd
│   ├── epoll_demo.c   # epoll event loop
│   └── *.cpp          # C++ examples
│
├── linux/             # Linux kernel source
├── qemu -> ...        # Symlink to QEMU source
└── start_qemu.sh      # Start QEMU VM
```

## Running Tests

```bash
# In guest - setup
mount -t 9p -o trans=virtio utils /mnt/utils
mount -t 9p -o trans=virtio tests /mnt/tests
bash /mnt/utils/bind_vfio.sh

# Run DMA test
/mnt/tests/vfio/test_edu_dma

# Run interrupt test  
python3 /mnt/tests/interrupts/test_msix.py

# Run NVMe test
/mnt/tests/nvme/test_nvme
```

## Building

```bash
# Compile C++ tests on host (binaries shared via virtfs)
cd tests/vfio && g++ -o test_edu_dma test_edu_dma.cpp

# Build kernel module (requires kernel headers)
cd drivers && make KDIR=/path/to/linux
```

## Requirements

- QEMU with custom edu-pci device (in qemu/)
- Linux kernel with VFIO and Intel IOMMU support
- Guest: Debian or similar with Python3, gcc/g++

## Exit QEMU

Press `Ctrl-A X`
