#!/usr/bin/env python3
import mmap, struct, os, subprocess

# Enable bus mastering on the device
subprocess.run(["setpci", "-s", "00:03.0", "COMMAND=0007"], check=True)

# Use low physical addresses that are definitely RAM
BAR0 = 0xfebd5000  # edu-pci BAR0 (check lspci -v)
SRC = 0x1000000    # 16MB - should be in RAM
DST = 0x1001000    # 16MB + 4KB

fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
bar = mmap.mmap(fd, 64, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=BAR0)
src = mmap.mmap(fd, 4096, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=SRC)
dst = mmap.mmap(fd, 4096, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=DST)

def w(off, val): bar.seek(off); bar.write(struct.pack('<I', val))
def r(off): bar.seek(off); return struct.unpack('<I', bar.read(4))[0]

print(f"Device ID: 0x{r(0x00):08X}")

src.seek(0); src.write(b'HELLO_DMA!' + b'\x00' * 54)
dst.seek(0); dst.write(b'\x00' * 64)

w(0x10, SRC)
w(0x14, DST)
w(0x18, 64)
print(f"SRC=0x{SRC:x} DST=0x{DST:x}")
w(0x1C, 1)

dst.seek(0)
print("Result:", dst.read(16))

bar.close(); src.close(); dst.close()
os.close(fd)
