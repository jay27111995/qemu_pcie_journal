#!/usr/bin/env python3
"""
Test VFIO INTx interrupts (legacy PCI interrupts).
Simpler than MSI-X, verifies basic VFIO interrupt path.
"""
import os
import fcntl
import struct
import mmap
import select

# VFIO constants
VFIO_TYPE = ord(';')
VFIO_BASE = 100
VFIO_GET_API_VERSION = VFIO_TYPE << 8 | VFIO_BASE
VFIO_CHECK_EXTENSION = VFIO_TYPE << 8 | (VFIO_BASE + 1)
VFIO_SET_IOMMU = VFIO_TYPE << 8 | (VFIO_BASE + 2)
VFIO_GROUP_GET_STATUS = VFIO_TYPE << 8 | (VFIO_BASE + 3)
VFIO_GROUP_SET_CONTAINER = VFIO_TYPE << 8 | (VFIO_BASE + 4)
VFIO_GROUP_GET_DEVICE_FD = VFIO_TYPE << 8 | (VFIO_BASE + 6)

VFIO_DEVICE_GET_INFO = VFIO_TYPE << 8 | (VFIO_BASE + 7)
VFIO_DEVICE_GET_REGION_INFO = VFIO_TYPE << 8 | (VFIO_BASE + 8)
VFIO_DEVICE_GET_IRQ_INFO = VFIO_TYPE << 8 | (VFIO_BASE + 9)
VFIO_DEVICE_SET_IRQS = VFIO_TYPE << 8 | (VFIO_BASE + 10)
VFIO_IOMMU_MAP_DMA = VFIO_TYPE << 8 | (VFIO_BASE + 13)

VFIO_TYPE1_IOMMU = 1

VFIO_PCI_INTX_IRQ_INDEX = 0
VFIO_PCI_MSI_IRQ_INDEX = 1
VFIO_PCI_MSIX_IRQ_INDEX = 2

VFIO_IRQ_SET_DATA_NONE = 1
VFIO_IRQ_SET_DATA_BOOL = 2
VFIO_IRQ_SET_DATA_EVENTFD = 4
VFIO_IRQ_SET_ACTION_MASK = 8
VFIO_IRQ_SET_ACTION_UNMASK = 16
VFIO_IRQ_SET_ACTION_TRIGGER = 32

REG_ID = 0x00
REG_IRQ_RAISE = 0x24

def main():
    # Open container
    container = os.open("/dev/vfio/vfio", os.O_RDWR)
    
    # Find IOMMU group for device
    group_path = "/sys/bus/pci/devices/0000:00:03.0/iommu_group"
    group_num = os.path.basename(os.readlink(group_path))
    
    # Open group
    group = os.open(f"/dev/vfio/{group_num}", os.O_RDWR)
    
    # Set IOMMU
    fcntl.ioctl(group, VFIO_GROUP_SET_CONTAINER, struct.pack('I', container))
    fcntl.ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU)
    
    # Get device fd (returns int when passed bytearray)
    device_name = "0000:00:03.0"
    device = fcntl.ioctl(group, VFIO_GROUP_GET_DEVICE_FD,
                         bytearray(device_name.encode() + b'\x00' * 32))
    print(f"Device fd: {device}")
    
    # Get device info
    dev_info = bytearray(32)
    struct.pack_into('<II', dev_info, 0, 32, 0)
    fcntl.ioctl(device, VFIO_DEVICE_GET_INFO, dev_info)
    _, flags, num_regions, num_irqs = struct.unpack_from('<IIII', dev_info, 0)
    print(f"Device info: flags=0x{flags:x}, regions={num_regions}, irqs={num_irqs}")
    
    # Get INTx IRQ info
    irq_info = bytearray(16)
    struct.pack_into('<II', irq_info, 0, 16, VFIO_PCI_INTX_IRQ_INDEX)
    fcntl.ioctl(device, VFIO_DEVICE_GET_IRQ_INFO, irq_info)
    _, _, flags, count = struct.unpack_from('<IIII', irq_info, 0)
    print(f"INTx: count={count}, flags=0x{flags:x}")
    
    if count == 0:
        print("No INTx support")
        return
    
    # Create eventfd
    import ctypes
    libc = ctypes.CDLL("libc.so.6", use_errno=True)
    eventfd = libc.eventfd(0, 0)
    if eventfd < 0:
        print("Failed to create eventfd")
        return
    print(f"Eventfd: {eventfd}")
    
    # Enable bus master
    region_info = bytearray(32)
    struct.pack_into('<II', region_info, 0, 32, 7)  # VFIO_PCI_CONFIG_REGION_INDEX
    fcntl.ioctl(device, VFIO_DEVICE_GET_REGION_INFO, region_info)
    _, _, config_offset = struct.unpack_from('<IQQ', region_info, 0)
    
    # Read command register and enable bus master
    os.lseek(device, config_offset + 4, os.SEEK_SET)
    cmd = struct.unpack('<H', os.read(device, 2))[0]
    os.lseek(device, config_offset + 4, os.SEEK_SET)
    os.write(device, struct.pack('<H', cmd | 0x0007))
    
    # Set up INTx with eventfd
    # argsz=32, flags, index, start, count, fd
    irq_set = bytearray(24)
    struct.pack_into('<IIIII', irq_set, 0, 
                     24,  # argsz
                     VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER,
                     VFIO_PCI_INTX_IRQ_INDEX,
                     0,  # start
                     1)  # count
    struct.pack_into('<i', irq_set, 20, eventfd)
    
    try:
        fcntl.ioctl(device, VFIO_DEVICE_SET_IRQS, irq_set)
        print("INTx enabled!")
    except OSError as e:
        print(f"Failed to enable INTx: {e}")
        return
    
    # Get BAR0 info
    region_info = bytearray(32)
    struct.pack_into('<II', region_info, 0, 32, 0)  # BAR0
    fcntl.ioctl(device, VFIO_DEVICE_GET_REGION_INFO, region_info)
    _, _, bar_size, bar_offset = struct.unpack_from('<IIQQ', region_info, 0)
    
    # mmap BAR0
    bar0 = mmap.mmap(device, bar_size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=bar_offset)
    
    # Read device ID
    bar0.seek(REG_ID)
    dev_id = struct.unpack('<I', bar0.read(4))[0]
    print(f"Device ID: 0x{dev_id:08x}")
    
    # Wait for interrupt with timeout
    print("\nWaiting for INTx interrupt (trigger via IRQ_RAISE)...")
    bar0.seek(REG_IRQ_RAISE)
    bar0.write(struct.pack('<I', 0))  # Trigger IRQ_RAISE (write any value)
    
    ready, _, _ = select.select([eventfd], [], [], 2.0)
    if ready:
        count = struct.unpack('<Q', os.read(eventfd, 8))[0]
        print(f"SUCCESS: Received {count} INTx interrupt(s)!")
    else:
        print("TIMEOUT: No interrupt received")
    
    bar0.close()
    os.close(eventfd)
    os.close(device)
    os.close(group)
    os.close(container)

if __name__ == "__main__":
    main()
