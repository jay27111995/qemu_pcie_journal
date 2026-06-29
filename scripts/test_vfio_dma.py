#!/usr/bin/env python3
"""
VFIO DMA test for edu-pci device.

Demonstrates:
1. Opening a device via VFIO
2. Setting up DMA mappings (IOVA = guest physical address)
3. Triggering device DMA and verifying data transfer

Key insight: Bus master must be enabled AFTER opening VFIO device fd,
because VFIO resets the device on open which clears the bus master bit.
"""
import os
import struct
import mmap
import fcntl
import ctypes
import subprocess

# VFIO ioctl numbers
VFIO_GET_API_VERSION = 0x3B64
VFIO_SET_IOMMU = 0x3B66
VFIO_GROUP_SET_CONTAINER = 0x3B68
VFIO_GROUP_GET_DEVICE_FD = 0x3B6A
VFIO_DEVICE_GET_REGION_INFO = 0x3B6C
VFIO_IOMMU_MAP_DMA = 0x3B71

VFIO_TYPE1_IOMMU = 1
VFIO_DMA_MAP_FLAG_READ = 1
VFIO_DMA_MAP_FLAG_WRITE = 2

# edu-pci register offsets
REG_ID = 0x00
REG_DMA_SRC = 0x10
REG_DMA_DST = 0x14
REG_DMA_LEN = 0x18
REG_DMA_CMD = 0x1C


def virt_to_phys(vaddr):
    """Get guest physical address from virtual address via /proc/self/pagemap."""
    PAGE_SIZE = 4096
    with open("/proc/self/pagemap", "rb") as f:
        f.seek((vaddr // PAGE_SIZE) * 8)
        entry = struct.unpack("Q", f.read(8))[0]
        if entry & (1 << 63):  # page present
            pfn = entry & ((1 << 55) - 1)
            return (pfn * PAGE_SIZE) + (vaddr % PAGE_SIZE)
    return None


def main():
    device = "0000:00:04.0"

    # --- Step 1: Open VFIO container and group ---
    group_link = os.readlink(f"/sys/bus/pci/devices/{device}/iommu_group")
    group_num = int(os.path.basename(group_link))
    print(f"Device {device} in IOMMU group {group_num}")

    container = os.open("/dev/vfio/vfio", os.O_RDWR)
    group = os.open(f"/dev/vfio/{group_num}", os.O_RDWR)

    fcntl.ioctl(group, VFIO_GROUP_SET_CONTAINER, struct.pack('i', container))
    fcntl.ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU)

    # --- Step 2: Get device fd and mmap BAR0 ---
    device_buf = bytearray(device.encode() + b'\x00' * 32)
    device_fd = fcntl.ioctl(group, VFIO_GROUP_GET_DEVICE_FD, device_buf)

    reg_info = bytearray(32)
    struct.pack_into('II', reg_info, 0, 32, 0)  # argsz, index
    fcntl.ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, reg_info)
    bar_size, bar_offset = struct.unpack_from('QQ', reg_info, 16)

    bar = mmap.mmap(device_fd, bar_size, mmap.MAP_SHARED,
                    mmap.PROT_READ | mmap.PROT_WRITE, offset=bar_offset)

    # --- Step 3: Enable bus master (MUST be after opening device fd) ---
    subprocess.run(["setpci", "-s", device, "COMMAND=0007"], check=True)

    # Helper functions for BAR access
    def read_reg(off):
        bar.seek(off)
        return struct.unpack('<I', bar.read(4))[0]

    def write_reg(off, val):
        bar.seek(off)
        bar.write(struct.pack('<I', val))

    print(f"Device ID: 0x{read_reg(REG_ID):08X}")

    # --- Step 4: Allocate and map DMA buffers ---
    PAGE_SIZE = 4096
    libc = ctypes.CDLL("libc.so.6")
    libc.mmap.restype = ctypes.c_void_p

    # Allocate page-aligned buffers
    src_va = libc.mmap(0, PAGE_SIZE, 3, 0x22, -1, 0)  # PROT_READ|WRITE, MAP_PRIVATE|ANON
    dst_va = libc.mmap(0, PAGE_SIZE, 3, 0x22, -1, 0)

    # Touch pages to ensure they're mapped
    src_buf = (ctypes.c_char * PAGE_SIZE).from_address(src_va)
    dst_buf = (ctypes.c_char * PAGE_SIZE).from_address(dst_va)
    src_buf[0] = b'X'
    dst_buf[0] = b'Y'

    # Get physical addresses (use as IOVA for identity mapping)
    src_phys = virt_to_phys(src_va)
    dst_phys = virt_to_phys(dst_va)
    if not src_phys or not dst_phys:
        print("ERROR: Could not get physical addresses")
        return

    # Map DMA buffers: IOVA = guest physical address
    # struct vfio_iommu_type1_dma_map { u32 argsz, flags; u64 vaddr, iova, size; }
    for va, iova in [(src_va, src_phys), (dst_va, dst_phys)]:
        dma_map = struct.pack('IIQQQ', 32,
                              VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
                              va, iova, PAGE_SIZE)
        fcntl.ioctl(container, VFIO_IOMMU_MAP_DMA, dma_map)

    print(f"SRC: va=0x{src_va:x} -> iova=0x{src_phys:x}")
    print(f"DST: va=0x{dst_va:x} -> iova=0x{dst_phys:x}")

    # --- Step 5: Write test data and trigger DMA ---
    test_data = b'HELLO VFIO DMA!\x00'
    src_buf[:len(test_data)] = test_data
    dst_buf[:64] = b'\x00' * 64

    write_reg(REG_DMA_SRC, src_phys & 0xFFFFFFFF)
    write_reg(REG_DMA_DST, dst_phys & 0xFFFFFFFF)
    write_reg(REG_DMA_LEN, 64)
    write_reg(REG_DMA_CMD, 1)  # Start DMA

    # --- Step 6: Verify result ---
    result = bytes(dst_buf[:16])
    print(f"Result: {result}")

    if result.startswith(b'HELLO VFIO DMA!'):
        print("SUCCESS: DMA transfer completed!")
    else:
        print("FAILED: Data mismatch")

    # Cleanup
    bar.close()
    os.close(device_fd)
    os.close(group)
    os.close(container)


if __name__ == "__main__":
    main()
