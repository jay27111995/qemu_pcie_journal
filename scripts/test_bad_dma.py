#!/usr/bin/env python3
"""Test DMA to invalid address - should trigger IOMMU fault"""
import os, struct, mmap, fcntl, subprocess

VFIO_GROUP_SET_CONTAINER = 0x3B68
VFIO_GROUP_GET_DEVICE_FD = 0x3B6A
VFIO_DEVICE_GET_REGION_INFO = 0x3B6C
VFIO_SET_IOMMU = 0x3B66
VFIO_TYPE1_IOMMU = 1

device = "0000:00:03.0"
group_link = os.readlink(f"/sys/bus/pci/devices/{device}/iommu_group")
group_num = int(os.path.basename(group_link))

container = os.open("/dev/vfio/vfio", os.O_RDWR)
group = os.open(f"/dev/vfio/{group_num}", os.O_RDWR)
fcntl.ioctl(group, VFIO_GROUP_SET_CONTAINER, struct.pack('i', container))
fcntl.ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU)

device_buf = bytearray(device.encode() + b'\x00' * 32)
device_fd = fcntl.ioctl(group, VFIO_GROUP_GET_DEVICE_FD, device_buf)

reg_info = bytearray(32)
struct.pack_into('II', reg_info, 0, 32, 0)
fcntl.ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, reg_info)
bar_size, bar_offset = struct.unpack_from('QQ', reg_info, 16)

bar = mmap.mmap(device_fd, bar_size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=bar_offset)

# Enable bus master
subprocess.run(["setpci", "-s", device, "COMMAND=0007"], check=True)

def write_reg(off, val):
    bar.seek(off)
    bar.write(struct.pack('<I', val))

# Try DMA from a wild unmapped address
WILD_ADDR = 0xDEADBEEF
print(f"Triggering DMA from unmapped address 0x{WILD_ADDR:X}...")
print("Check dmesg for IOMMU fault!")

write_reg(0x10, WILD_ADDR)  # DMA_SRC = wild address
write_reg(0x14, WILD_ADDR)  # DMA_DST = wild address  
write_reg(0x18, 64)         # DMA_LEN
write_reg(0x1C, 1)          # DMA_CMD = start

print("DMA triggered. Run: dmesg | tail -10")
