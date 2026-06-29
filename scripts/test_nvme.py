#!/usr/bin/env python3
"""
Test edu-nvme controller via VFIO.
Demonstrates NVMe submission/completion queue flow.
"""
import os
import mmap
import struct
import fcntl
import select
import ctypes

# VFIO constants
VFIO_SET_IOMMU = 0x3B66
VFIO_GROUP_SET_CONTAINER = 0x3B68
VFIO_GROUP_GET_DEVICE_FD = 0x3B6A
VFIO_DEVICE_GET_REGION_INFO = 0x3B6C
VFIO_DEVICE_SET_IRQS = 0x3B6E
VFIO_IOMMU_MAP_DMA = 0x3B71
VFIO_TYPE1_IOMMU = 1
VFIO_IRQ_SET_DATA_EVENTFD = (1 << 2)
VFIO_IRQ_SET_ACTION_TRIGGER = (1 << 5)
VFIO_PCI_MSIX_IRQ_INDEX = 2

# NVMe registers
NVME_CAP    = 0x00
NVME_VS     = 0x08
NVME_CC     = 0x14
NVME_CSTS   = 0x1C
NVME_AQA    = 0x24
NVME_ASQ    = 0x28
NVME_ACQ    = 0x30
NVME_SQ0TDBL = 0x1000
NVME_CQ0HDBL = 0x1004

# NVMe opcodes
NVME_CMD_IDENTIFY = 0x06
NVME_CMD_READ     = 0x02
NVME_CMD_WRITE    = 0x01

QUEUE_SIZE = 16
PAGE_SIZE = 4096

def main():
    device = "0000:00:03.0"  # edu-nvme device

    # Open VFIO
    group_num = int(os.path.basename(os.readlink(f"/sys/bus/pci/devices/{device}/iommu_group")))
    container = os.open("/dev/vfio/vfio", os.O_RDWR)
    group = os.open(f"/dev/vfio/{group_num}", os.O_RDWR)
    fcntl.ioctl(group, VFIO_GROUP_SET_CONTAINER, struct.pack('i', container))
    fcntl.ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU)

    device_fd = fcntl.ioctl(group, VFIO_GROUP_GET_DEVICE_FD,
                            bytearray(device.encode() + b'\x00' * 32))
    print(f"Device fd: {device_fd}")

    # Get BAR0 info
    bar_info = bytearray(40)
    struct.pack_into('III', bar_info, 0, 40, 0, 0)
    fcntl.ioctl(device_fd, VFIO_DEVICE_GET_REGION_INFO, bar_info)
    _, _, _, _, bar_size, bar_offset = struct.unpack_from('IIIIQQ', bar_info)
    print(f"BAR0: size={bar_size}, offset=0x{bar_offset:x}")

    # mmap BAR0
    bar0 = mmap.mmap(device_fd, bar_size, mmap.MAP_SHARED,
                     mmap.PROT_READ | mmap.PROT_WRITE, offset=bar_offset)

    def read_reg(off):
        bar0.seek(off)
        return struct.unpack('<I', bar0.read(4))[0]

    def write_reg(off, val):
        bar0.seek(off)
        bar0.write(struct.pack('<I', val))

    def read_reg64(off):
        bar0.seek(off)
        return struct.unpack('<Q', bar0.read(8))[0]

    def write_reg64(off, val):
        bar0.seek(off)
        bar0.write(struct.pack('<Q', val))

    # Read capabilities
    cap = read_reg64(NVME_CAP)
    vs = read_reg(NVME_VS)
    print(f"CAP: 0x{cap:016x}")
    print(f"Version: {(vs >> 16) & 0xFF}.{(vs >> 8) & 0xFF}")

    # Setup MSI-X
    libc = ctypes.CDLL("libc.so.6", use_errno=True)
    eventfd = libc.eventfd(0, 0)
    print(f"Eventfd: {eventfd}")

    irq_info = bytearray(16)
    struct.pack_into('III', irq_info, 0, 16, 0, VFIO_PCI_MSIX_IRQ_INDEX)
    fcntl.ioctl(device_fd, 0x3B6D, irq_info)  # GET_IRQ_INFO
    _, _, flags, irq_count = struct.unpack('IIII', irq_info)
    print(f"MSI-X vectors: {irq_count}")

    irq_set = bytearray(24)
    struct.pack_into('IIIIIi', irq_set, 0, 24,
                     VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER,
                     VFIO_PCI_MSIX_IRQ_INDEX, 0, 1, eventfd)
    fcntl.ioctl(device_fd, VFIO_DEVICE_SET_IRQS, irq_set)
    print("MSI-X enabled")

    # Allocate DMA memory for queues and data
    # SQ: 64 bytes per entry * 16 = 1024 bytes
    # CQ: 16 bytes per entry * 16 = 256 bytes
    # Data: 4KB
    dma_size = PAGE_SIZE * 4
    dma_mem = mmap.mmap(-1, dma_size, mmap.MAP_SHARED | 0x20,  # MAP_ANONYMOUS | MAP_HUGETLB fallback
                        mmap.PROT_READ | mmap.PROT_WRITE)

    # Get physical address (use VFIO DMA map)
    dma_iova = 0x100000  # IOVA we choose
    dma_map = struct.pack('QQQI', 32, dma_iova, dma_size,
                          ctypes.addressof(ctypes.c_char.from_buffer(dma_mem)))

    # Map: argsz, flags, vaddr, iova, size
    dma_map_buf = bytearray(40)
    vaddr = ctypes.addressof(ctypes.c_char.from_buffer(dma_mem))
    struct.pack_into('IIQqq', dma_map_buf, 0, 40, 3, vaddr, dma_iova, dma_size)
    fcntl.ioctl(container, VFIO_IOMMU_MAP_DMA, dma_map_buf)
    print(f"DMA mapped: IOVA=0x{dma_iova:x}, size={dma_size}")

    sq_iova = dma_iova
    cq_iova = dma_iova + PAGE_SIZE
    data_iova = dma_iova + PAGE_SIZE * 2

    # Configure controller
    # Set Admin Queue Attributes: ACQS=15 (16 entries), ASQS=15
    write_reg(NVME_AQA, ((QUEUE_SIZE - 1) << 16) | (QUEUE_SIZE - 1))

    # Set Admin Submission Queue base address
    write_reg64(NVME_ASQ, sq_iova)

    # Set Admin Completion Queue base address
    write_reg64(NVME_ACQ, cq_iova)

    print(f"AQA: 0x{read_reg(NVME_AQA):08x}")
    print(f"ASQ: 0x{read_reg64(NVME_ASQ):016x}")
    print(f"ACQ: 0x{read_reg64(NVME_ACQ):016x}")

    # Enable controller
    write_reg(NVME_CC, 0x00460001)  # EN=1, CSS=0, MPS=0, IOSQES=6, IOCQES=4
    print("Enabling controller...")

    # Wait for ready
    for _ in range(100):
        csts = read_reg(NVME_CSTS)
        if csts & 1:
            break
    print(f"CSTS: 0x{csts:08x} (ready={csts & 1})")

    # === Test 1: Identify Controller ===
    print("\n=== Identify Controller ===")

    # Build identify command (64 bytes)
    cmd = bytearray(64)
    struct.pack_into('<BBHIQQQQIIIIII', cmd, 0,
        NVME_CMD_IDENTIFY,  # opcode
        0,                  # flags
        1,                  # cid
        0,                  # nsid
        0, 0,               # rsvd, mptr
        data_iova,          # prp1 (where to write identify data)
        0,                  # prp2
        1,                  # cdw10: CNS=1 (identify controller)
        0, 0, 0, 0, 0)

    # Write command to SQ
    dma_mem.seek(0)
    dma_mem.write(cmd)

    # Ring SQ doorbell
    write_reg(NVME_SQ0TDBL, 1)
    print("Command submitted, waiting for completion...")

    # Wait for interrupt
    r, _, _ = select.select([eventfd], [], [], 2.0)
    if r:
        os.read(eventfd, 8)
        print("Interrupt received!")

        # Read completion
        dma_mem.seek(PAGE_SIZE)
        cqe = dma_mem.read(16)
        result, rsvd, sq_head, sq_id, cid, status = struct.unpack('<IIHHHH', cqe)
        print(f"Completion: cid={cid}, status=0x{status:04x}, sq_head={sq_head}")

        # Read identify data
        dma_mem.seek(PAGE_SIZE * 2)
        id_data = dma_mem.read(64)
        vid = struct.unpack('<H', id_data[0:2])[0]
        sn = id_data[4:24].decode('ascii', errors='ignore').strip('\x00')
        mn = id_data[24:64].decode('ascii', errors='ignore').strip('\x00')
        print(f"Vendor ID: 0x{vid:04x}")
        print(f"Serial: {sn}")
        print(f"Model: {mn}")

        # Update CQ head doorbell
        write_reg(NVME_CQ0HDBL, 1)
    else:
        print("TIMEOUT!")

    # === Test 2: Write Data ===
    print("\n=== Write Data ===")

    # Write test pattern to data buffer
    test_data = b"Hello NVMe! This is a test write."
    dma_mem.seek(PAGE_SIZE * 2)
    dma_mem.write(test_data.ljust(512, b'\x00'))

    # Build write command
    cmd = bytearray(64)
    struct.pack_into('<BBHIQQQQIIIIII', cmd, 0,
        NVME_CMD_WRITE,     # opcode
        0,                  # flags
        2,                  # cid
        1,                  # nsid
        0, 0,               # rsvd, mptr
        data_iova,          # prp1
        0,                  # prp2
        0,                  # cdw10: LBA low = 0
        0,                  # cdw11: LBA high = 0
        0,                  # cdw12: NLB = 0 (1 block)
        0, 0, 0)

    dma_mem.seek(64)  # Next SQ slot
    dma_mem.write(cmd)
    write_reg(NVME_SQ0TDBL, 2)

    r, _, _ = select.select([eventfd], [], [], 2.0)
    if r:
        os.read(eventfd, 8)
        dma_mem.seek(PAGE_SIZE + 16)  # Next CQ slot
        cqe = dma_mem.read(16)
        _, _, _, _, cid, status = struct.unpack('<IIHHHH', cqe)
        print(f"Write complete: cid={cid}, status=0x{status:04x}")
        write_reg(NVME_CQ0HDBL, 2)
    else:
        print("Write TIMEOUT!")

    # === Test 3: Read Data Back ===
    print("\n=== Read Data ===")

    # Clear data buffer
    dma_mem.seek(PAGE_SIZE * 2)
    dma_mem.write(b'\x00' * 512)

    # Build read command
    cmd = bytearray(64)
    struct.pack_into('<BBHIQQQQIIIIII', cmd, 0,
        NVME_CMD_READ,      # opcode
        0,                  # flags
        3,                  # cid
        1,                  # nsid
        0, 0,               # rsvd, mptr
        data_iova,          # prp1
        0,                  # prp2
        0,                  # cdw10: LBA low = 0
        0,                  # cdw11: LBA high = 0
        0,                  # cdw12: NLB = 0 (1 block)
        0, 0, 0)

    dma_mem.seek(128)  # Next SQ slot
    dma_mem.write(cmd)
    write_reg(NVME_SQ0TDBL, 3)

    r, _, _ = select.select([eventfd], [], [], 2.0)
    if r:
        os.read(eventfd, 8)
        dma_mem.seek(PAGE_SIZE + 32)  # Next CQ slot
        cqe = dma_mem.read(16)
        _, _, _, _, cid, status = struct.unpack('<IIHHHH', cqe)
        print(f"Read complete: cid={cid}, status=0x{status:04x}")

        # Check data
        dma_mem.seek(PAGE_SIZE * 2)
        read_data = dma_mem.read(len(test_data))
        print(f"Read data: {read_data}")
        if read_data == test_data:
            print("SUCCESS: Data matches!")
        else:
            print("FAIL: Data mismatch!")
        write_reg(NVME_CQ0HDBL, 3)
    else:
        print("Read TIMEOUT!")

    # Cleanup
    bar0.close()
    dma_mem.close()
    os.close(eventfd)
    os.close(device_fd)
    os.close(group)
    os.close(container)

if __name__ == "__main__":
    main()
