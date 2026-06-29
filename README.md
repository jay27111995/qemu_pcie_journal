# QEMU PCIe Learning Journal

## Overview

Scripts and examples for learning PCIe, VFIO, DMA, and Linux kernel driver development using QEMU.

## Quick Start

```bash
./start_qemu.sh  # Start QEMU with edu-pci devices
# Inside guest:
mount -t 9p -o trans=virtio scripts /mnt/scripts
```

## Scripts

### VFIO & DMA

| Script | Description |
|--------|-------------|
| `test_vfio.py` | Basic VFIO device access - open device, map BAR, read registers |
| `test_vfio_dma.py` | VFIO DMA mapping and transfer between host buffers |
| `test_edu_dma.cpp` | C++ VFIO DMA test with MSI-X interrupt handling |
| `test_bad_dma.py` | Test IOMMU fault handling with invalid DMA addresses |
| `dma.py` | DMA utility functions |

### Interrupts

| Script | Description |
|--------|-------------|
| `test_intx.py` | Legacy INTx interrupt handling via VFIO |
| `test_msix.py` | MSI-X interrupt setup and handling |
| `test_msix_direct.py` | Direct MSI-X programming |
| `test_msix_devmem.py` | MSI-X via /dev/mem |

### NVMe

| Script | Description |
|--------|-------------|
| `test_nvme.py` | NVMe controller test - submission/completion queues |
| `test_nvme.cpp` | C++ NVMe test with full read/write/identify commands |

### Advanced PCIe

| Script | Description |
|--------|-------------|
| `test_ats.cpp` | ATS (Address Translation Services) - device IOMMU translation requests |
| `test_p2p.sh` | P2P DMA - device-to-device transfer without CPU |

### Kernel Driver

| Script | Description |
|--------|-------------|
| `edu_pci.c` | Linux kernel driver for edu-pci device (probe, BAR mapping, registers) |
| `Makefile` | Build the kernel module |

### C++ Examples

| Script | Description |
|--------|-------------|
| `eventfd_demo.c` | Linux eventfd for thread signaling |
| `epoll_demo.c` | epoll + eventfd + socket event loop |
| `friend_demo.cpp` | C++ friend function example |
| `friend_mult.cpp` | C++ friend operator overloading |
| `unique_ptr_demo.cpp` | C++ smart pointer and move semantics |

### Utilities

| Script | Description |
|--------|-------------|
| `bind_vfio.sh` | Bind PCI device to vfio-pci driver |
| `setup_mount.sh` | Mount virtfs shared folder |
| `test_regs.py` | Basic register read/write test |
| `test_usb.py` | USB device test |

## Requirements

- QEMU with edu-pci and edu-nvme devices
- Linux kernel with VFIO and Intel IOMMU support
- Guest: Debian or similar with Python3, g++

## Building Kernel Module

```bash
# On host (with kernel source)
cd scripts
make KDIR=/path/to/linux

# In guest
insmod /mnt/scripts/edu_pci.ko
```
