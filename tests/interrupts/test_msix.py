#!/usr/bin/env python3
"""
Test MSI-X interrupts on edu-pci device via VFIO.

Shows how to:
1. Enable MSI-X on a VFIO device
2. Set up eventfd for interrupt notification
3. Trigger interrupt and wait for it
"""
import os
import struct
import mmap
import fcntl
import select
import subprocess

# VFIO ioctls
VFIO_SET_IOMMU = 0x3B66
VFIO_GROUP_SET_CONTAINER = 0x3B68
VFIO_GROUP_GET_DEVICE_FD = 0x3B6A
VFIO_DEVICE_GET_REGION_INFO = 0x3B6C
VFIO_DEVICE_SET_IRQS = 0x3B6E
VFIO_TYPE1_IOMMU = 1

# VFIO IRQ flags
VFIO_IRQ_SET_DATA_NONE = (1 << 0)
VFIO_IRQ_SET_DATA_EVENTFD = (1 << 2)
VFIO_IRQ_SET_ACTION_TRIGGER = (1 << 5)

# IRQ index
VFIO_PCI_MSIX_IRQ_INDEX = 2

REG_IRQ_RAISE = 0x24

def main():
    device = "0000:00:03.0"

    # Open VFIO
    group_num = int(os.path.basename(os.readlink(f"/sys/bus/pci/devices/{device}/iommu_group")))
    container = os.open("/dev/vfio/vfio", os.O_RDWR)
    group = os.open(f"/dev/vfio/{group_num}", os.O_RDWR)
    fcntl.ioctl(group, VFIO_GROUP_SET_CONTAINER, struct.pack('i', container))
    fcntl.ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU)

    device_fd = fcntl.ioctl(group, VFIO_GROUP_GET_DEVICE_FD,
                            bytearray(device.encode() + b'\x00' * 32))
    print(f"Device fd: {device_fd}")

    # Get BAR0
    reg_info = bytearray(40)
    struct.pack_into('II', reg_info, 0, 40, 0)  # argsz=40, index=0
    fcntl.ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, reg_info)
    argsz, flags, index, cap_off, bar_size, bar_offset = struct.unpack_from('IIIIQQ', reg_info, 0)
    print(f"BAR0: size={bar_size}, offset=0x{bar_offset:x}, flags=0x{flags:x}")

    bar = mmap.mmap(device_fd, bar_size, mmap.MAP_SHARED,
                    mmap.PROT_READ | mmap.PROT_WRITE, offset=bar_offset)

    subprocess.run(["setpci", "-s", device, "COMMAND=0007"], check=True)

    def write_reg(off, val):
        bar.seek(off)
        bar.write(struct.pack('<I', val))

    # Create eventfd for MSI-X vector 0
    import ctypes
    libc = ctypes.CDLL("libc.so.6")
    eventfd = libc.eventfd(0, 0)
    print(f"Eventfd: {eventfd}")

    # First query how many MSI-X vectors the device has
    VFIO_DEVICE_GET_IRQ_INFO = 0x3B6D
    # struct vfio_irq_info { argsz, flags, index, count }
    irq_info = bytearray(16)
    struct.pack_into('III', irq_info, 0, 16, 0, VFIO_PCI_MSIX_IRQ_INDEX)
    fcntl.ioctl(device_fd, VFIO_DEVICE_GET_IRQ_INFO, irq_info)
    argsz, irq_flags, irq_index, irq_count = struct.unpack('IIII', irq_info)
    print(f"MSI-X: count={irq_count}, flags=0x{irq_flags:x}")

    # Check BAR2 (MSI-X table) 
    # struct vfio_region_info { argsz, flags, index, cap_offset, size, offset }
    bar2_info = bytearray(40)
    struct.pack_into('III', bar2_info, 0, 40, 0, 2)  # argsz=40, flags=0, index=2
    fcntl.ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, bar2_info)
    bar2_argsz, flags2, bar2_index, bar2_cap, bar2_size, bar2_offset = struct.unpack_from('IIIIQQ', bar2_info, 0)
    print(f"BAR2: argsz={bar2_argsz}, index={bar2_index}, cap={bar2_cap}, size={bar2_size}, offset=0x{bar2_offset:x}, flags=0x{flags2:x}")
    
    if bar2_size > 0:
        if bar2_offset == 0:
            print("WARNING: BAR2 offset is 0 (same as BAR0?)")
        try:
            bar2 = mmap.mmap(device_fd, bar2_size, mmap.MAP_SHARED,
                            mmap.PROT_READ | mmap.PROT_WRITE, offset=bar2_offset)
            bar2.seek(0)
            addr_lo, addr_hi, data, ctrl = struct.unpack('<IIII', bar2.read(16))
            print(f"MSI-X entry 0: addr=0x{addr_hi:08x}{addr_lo:08x}, data=0x{data:x}, ctrl=0x{ctrl:x}")
            bar2.close()
        except Exception as e:
            print(f"Failed to mmap BAR2: {e}")
    else:
        print("BAR2 size is 0")

    if irq_count == 0:
        print("ERROR: Device has no MSI-X vectors")
        return

    # Enable MSI-X with eventfds - VFIO will call pci_alloc_irq_vectors internally
    print("\nEnabling MSI-X with eventfd...")
    argsz = 20 + 4 * irq_count  # header + fds for all vectors
    irq_set = bytearray(argsz)
    struct.pack_into('IIIII', irq_set, 0, argsz,
                     VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER,
                     VFIO_PCI_MSIX_IRQ_INDEX, 0, irq_count)
    # Vector 0 = eventfd, rest = -1 (disabled)
    struct.pack_into('i', irq_set, 20, eventfd)
    for i in range(1, irq_count):
        struct.pack_into('i', irq_set, 20 + 4*i, -1)
    
    try:
        fcntl.ioctl(device_fd, VFIO_DEVICE_SET_IRQS, irq_set)
        print("MSI-X enabled!")
    except OSError as e:
        print(f"Failed to enable MSI-X: {e}")
        print("This may be due to nested virtualization limitations with interrupt remapping")
        return

    # Trigger interrupt by writing to IRQ_RAISE register
    print("\nTriggering MSI-X vector 0...")
    
    # First check what VFIO programmed in the MSI-X table
    if bar2_size > 0:
        bar2 = mmap.mmap(device_fd, bar2_size, mmap.MAP_SHARED,
                        mmap.PROT_READ | mmap.PROT_WRITE, offset=bar2_offset)
        bar2.seek(0)
        addr_lo, addr_hi, data, ctrl = struct.unpack('<IIII', bar2.read(16))
        print(f"MSI-X entry 0 after enable: addr=0x{addr_hi:08x}{addr_lo:08x}, data=0x{data:x}, ctrl=0x{ctrl:x}")
        bar2.close()
    
    write_reg(REG_IRQ_RAISE, 0)

    # Wait for interrupt with timeout
    print("Waiting for interrupt...")
    r, _, _ = select.select([eventfd], [], [], 2.0)
    if r:
        # Read eventfd to clear it
        data = os.read(eventfd, 8)
        count = struct.unpack('Q', data)[0]
        print(f"SUCCESS: Received interrupt! (count={count})")
    else:
        print("TIMEOUT: No interrupt received")

    # Cleanup
    bar.close()
    os.close(eventfd)
    os.close(device_fd)
    os.close(group)
    os.close(container)

if __name__ == "__main__":
    main()
