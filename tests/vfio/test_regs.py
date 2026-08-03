#!/usr/bin/env python3
"""Test edu-pci device registers (no DMA)"""
import mmap, struct, os

# Get BAR0 address: lspci -d 1234:ed01 -v | grep "Region 0"
BAR0 = 0xfebd5000

fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
bar = mmap.mmap(fd, 64, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=BAR0)

def w(off, val): bar.seek(off); bar.write(struct.pack('<I', val))
def r(off): bar.seek(off); return struct.unpack('<I', bar.read(4))[0]

print(f"Device ID:  0x{r(0x00):08X}")  # expect 0x0ED00001
print(f"Status:     0x{r(0x0C):08X}")

# Test scratch register
w(0x04, 0xDEADBEEF)
print(f"Scratch:    0x{r(0x04):08X}")  # expect 0xDEADBEEF

# Test factorial
w(0x08, 5)
print(f"5! =        {r(0x08)}")  # expect 120

w(0x08, 10)
print(f"10! =       {r(0x08)}")  # expect 3628800

bar.close()
os.close(fd)
