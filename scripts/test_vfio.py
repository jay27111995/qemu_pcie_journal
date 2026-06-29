#!/usr/bin/env python3
"""
VFIO example for edu-pci device

Setup (run once):
  # Find device
  lspci -d 1234:ed01
  # Unbind from current driver
  echo 0000:00:03.0 > /sys/bus/pci/devices/0000:00:03.0/driver/unbind
  # Bind to vfio-pci
  echo 1234 ed01 > /sys/bus/pci/drivers/vfio-pci/new_id
  # Check group
  ls /dev/vfio/
"""
import os
import struct
import mmap
import fcntl

# VFIO ioctl numbers
VFIO_GET_API_VERSION = 0x3B64
VFIO_CHECK_EXTENSION = 0x3B65
VFIO_SET_IOMMU = 0x3B66
VFIO_GROUP_GET_STATUS = 0x3B67
VFIO_GROUP_SET_CONTAINER = 0x3B68
VFIO_GROUP_GET_DEVICE_FD = 0x3B6A
VFIO_DEVICE_GET_INFO = 0x3B6B
VFIO_DEVICE_GET_REGION_INFO = 0x3B6C

VFIO_TYPE1_IOMMU = 1
VFIO_GROUP_FLAGS_VIABLE = 1

# Structures
def vfio_group_status():
    # struct vfio_group_status { __u32 argsz; __u32 flags; }
    return struct.pack('II', 8, 0)

def vfio_device_info():
    # struct vfio_device_info { __u32 argsz; __u32 flags; __u32 num_regions; __u32 num_irqs; }
    return bytearray(16)

def vfio_region_info(index):
    # struct vfio_region_info { __u32 argsz; __u32 flags; __u32 index; __u32 cap_offset;
    #                          __u64 size; __u64 offset; }
    buf = bytearray(32)
    struct.pack_into('IIII', buf, 0, 32, 0, index, 0)
    return buf

def main():
    device = "0000:00:03.0"
    group_num = None
    
    # Find IOMMU group
    group_link = os.readlink(f"/sys/bus/pci/devices/{device}/iommu_group")
    group_num = int(os.path.basename(group_link))
    print(f"Device {device} is in IOMMU group {group_num}")

    # Open container
    container = os.open("/dev/vfio/vfio", os.O_RDWR)
    print(f"Container fd: {container}")

    # Check API version
    api_ver = fcntl.ioctl(container, VFIO_GET_API_VERSION)
    print(f"VFIO API version: {api_ver}")

    # Open group
    group = os.open(f"/dev/vfio/{group_num}", os.O_RDWR)
    print(f"Group fd: {group}")

    # Check group is viable
    status = bytearray(vfio_group_status())
    fcntl.ioctl(group, VFIO_GROUP_GET_STATUS, status)
    flags = struct.unpack_from('II', status)[1]
    if not (flags & VFIO_GROUP_FLAGS_VIABLE):
        print("Group not viable! All devices must be bound to vfio-pci")
        return

    # Set container for group
    container_bytes = struct.pack('i', container)
    fcntl.ioctl(group, VFIO_GROUP_SET_CONTAINER, container_bytes)
    print("Group attached to container")

    # Set IOMMU type
    fcntl.ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU)
    print("IOMMU type set")

    # Get device fd - need mutable buffer
    device_buf = bytearray(device.encode() + b'\x00' * 32)
    device_fd = fcntl.ioctl(group, VFIO_GROUP_GET_DEVICE_FD, device_buf)
    print(f"Device fd: {device_fd}")

    # Get device info
    dev_info = vfio_device_info()
    struct.pack_into('I', dev_info, 0, 16)  # argsz
    fcntl.ioctl(device_fd, VFIO_DEVICE_GET_INFO, dev_info)
    num_regions = struct.unpack_from('IIII', dev_info)[2]
    print(f"Device has {num_regions} regions")

    # Get BAR0 info (region 0)
    reg_info = vfio_region_info(0)
    fcntl.ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, reg_info)
    flags, index, _, size, offset = struct.unpack_from('IIIQQ', reg_info)
    print(f"BAR0: size={size}, offset=0x{offset:x}")

    # mmap BAR0
    bar = mmap.mmap(device_fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=offset)
    print("BAR0 mapped!")

    # Read/write helpers
    def r(off):
        bar.seek(off)
        return struct.unpack('<I', bar.read(4))[0]
    def w(off, val):
        bar.seek(off)
        bar.write(struct.pack('<I', val))

    # Test device
    print(f"\nDevice ID:  0x{r(0x00):08X}")
    print(f"Status:     0x{r(0x0C):08X}")
    
    w(0x04, 0xCAFEBABE)
    print(f"Scratch:    0x{r(0x04):08X}")
    
    w(0x08, 7)
    print(f"7! =        {r(0x08)}")

    # Cleanup
    bar.close()
    os.close(device_fd)
    os.close(group)
    os.close(container)
    print("\nDone!")

if __name__ == "__main__":
    main()
