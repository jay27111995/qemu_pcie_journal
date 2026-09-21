# QEMU PCIe Learning Journal

## Overview

Scripts and examples for learning PCIe, VFIO, DMA, and Linux kernel driver development using QEMU.

## Quick Start

```bash
./start_qemu.sh  # Start QEMU with edu-pci devices

# Inside guest:
mkdir -p /mnt/tests /mnt/utils
mount -t 9p -o trans=virtio tests /mnt/tests
mount -t 9p -o trans=virtio utils /mnt/utils
/mnt/utils/bind_vfio.sh

# Run tests
/mnt/tests/vfio/test_edu_dma      # Simple DMA + MSI-X
/mnt/tests/vfio/test_ring_dma     # Descriptor ring DMA
/mnt/tests/vfio/test_crypto       # AES crypto engine
```

## Custom QEMU Devices

Source files in `qemu_devices/` (symlinked to QEMU source tree):

| File | Description |
|------|-------------|
| `edu_pci.c` | Educational PCIe device with SR-IOV, MSI-X, DMA |
| `edu_nvme.c` | Simplified NVMe controller for learning |
| `dev-edu.c` | USB device example |
| `dev-edu-serial.c` | USB serial device |

### edu-pci Device (edu_pci.c)

**BAR0** - Simple DMA (64 bytes):
| Offset | Register | Description |
|--------|----------|-------------|
| 0x00 | ID | 0x0ED00001 (PF) / 0x0ED00002 (VF) |
| 0x04 | Scratch | Read/write test register |
| 0x08 | Factorial | Write n, read n! |
| 0x0C | Status | Bit 0: ready, Bit 1: DMA busy |
| 0x10 | DMA_SRC | DMA source IOVA |
| 0x14 | DMA_DST | DMA destination IOVA |
| 0x18 | DMA_LEN | Transfer length |
| 0x1C | DMA_CMD | Write 1 to start, MSI-X on completion |
| 0x20 | VF_ID | VF number (0 for PF) |
| 0x24 | IRQ_RAISE | Write vector number to trigger MSI-X |

**BAR4** - Descriptor Ring DMA (64 bytes):
| Offset | Register | Description |
|--------|----------|-------------|
| 0x00 | RING_ID | 0x0ED0RING |
| 0x04 | RING_STATUS | Bit 0: enabled, Bit 1: running |
| 0x08 | RING_ADDR_LO | Ring base address (low) |
| 0x0C | RING_ADDR_HI | Ring base address (high) |
| 0x10 | RING_SIZE | Number of descriptors (max 256) |
| 0x14 | RING_HEAD | Device read pointer (RO) |
| 0x18 | RING_TAIL | Host write pointer (doorbell) |
| 0x1C | RING_CTRL | Bit 0: enable ring |

**BAR5** - AES Crypto Engine (64 bytes):
| Offset | Register | Description |
|--------|----------|-------------|
| 0x00 | CRYPTO_ID | 0x0ED0AE55 |
| 0x04 | STATUS | Bit 0: ready, Bit 1: busy |
| 0x08-0x0C | SRC | Source IOVA (64-bit) |
| 0x10-0x14 | DST | Destination IOVA (64-bit) |
| 0x18 | LEN | Data length (multiple of 16) |
| 0x1C | CMD | Bit 0: start, Bit 1: encrypt |
| 0x20-0x2F | KEY | 128-bit AES key |
| 0x30-0x3F | IV | 128-bit IV for CBC mode |

**MSI-X Vectors**:
- Vector 0: Simple DMA completion
- Vector 1: Ring DMA completion  
- Vector 2: Crypto completion

**Features**: SR-IOV (2 VFs), PCIe FLR, ATS simulation

## Directory Structure

```
qemu_learn/
├── qemu_devices/      # Custom QEMU device source files
│   ├── edu_pci.c      # PCIe device (DMA, ring, crypto)
│   ├── edu_nvme.c     # NVMe controller
│   ├── dev-edu.c      # USB device
│   └── dev-edu-serial.c
│
├── drivers/           # Kernel modules
│   ├── edu_pci.c      # Linux kernel driver for edu-pci
│   └── Makefile
│
├── tests/
│   ├── vfio/          # VFIO userspace tests
│   │   ├── test_edu_dma.cpp    # Simple DMA + MSI-X
│   │   ├── test_ring_dma.cpp   # Descriptor ring DMA
│   │   ├── test_crypto.cpp     # AES-128-CBC crypto
│   │   ├── test_vfio.py        # Basic BAR access
│   │   ├── test_vfio_dma.py    # DMA mapping (Python)
│   │   └── test_bad_dma.py     # IOMMU fault handling
│   │
│   ├── interrupts/    # Interrupt tests
│   │   ├── test_intx.py        # Legacy INTx
│   │   ├── test_msix.py        # MSI-X via VFIO
│   │   └── test_msix_direct.py # Direct MSI-X programming
│   │
│   └── nvme/          # NVMe tests
│       └── test_nvme.cpp       # NVMe queue test
│
├── utils/             # Utility scripts
│   ├── bind_vfio.sh   # Bind device to vfio-pci
│   └── mount_all.sh   # Mount all virtfs shares
│
├── examples/          # C/C++ learning examples
│
├── linux/             # Linux kernel source (not tracked)
├── qemu -> ...        # Symlink to QEMU source
└── start_qemu.sh      # Start QEMU VM
```

## Debug Output

Set `EDU_DEBUG` environment variable before starting QEMU:

```bash
EDU_DEBUG=0 ./start_qemu.sh  # Quiet (default)
EDU_DEBUG=1 ./start_qemu.sh  # TLP traces (BAR, DMA, MSI)
EDU_DEBUG=2 ./start_qemu.sh  # + Config space traces
EDU_DEBUG=3 ./start_qemu.sh  # + Ring DMA details
```

## Building

```bash
# Rebuild QEMU after modifying devices
source /opt/rh/gcc-toolset-12/enable
cd /repo/eelljay/qemu/build && ninja

# Compile C++ tests (binaries shared via virtfs)
cd tests/vfio && g++ -o test_edu_dma test_edu_dma.cpp
cd tests/vfio && g++ -o test_ring_dma test_ring_dma.cpp
cd tests/vfio && g++ -o test_crypto test_crypto.cpp
```

## Exit QEMU

Press `Ctrl-A X`
