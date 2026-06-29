#!/usr/bin/env python3
"""
Test MSI-X by directly accessing device registers via /dev/mem.
Bypasses VFIO to test if MSI-X hardware works.
"""
import mmap
import os
import struct
import subprocess

# Get BAR addresses from lspci
result = subprocess.run(["lspci", "-v", "-s", "00:03.0"], capture_output=True, text=True)
print(result.stdout)

# Find BAR0 and BAR2 addresses
bar0_addr = None
bar2_addr = None
for line in result.stdout.split('\n'):
    if 'Memory at' in line:
        addr = int(line.split('Memory at ')[1].split()[0], 16)
        if '64-bit' in line and bar0_addr is None:
            bar0_addr = addr
        elif '32-bit' in line:
            bar2_addr = addr

if not bar0_addr:
    print("Could not find BAR0 address")
    exit(1)

print(f"BAR0: 0x{bar0_addr:x}")
print(f"BAR2: 0x{bar2_addr:x}" if bar2_addr else "BAR2: not found")

# Enable bus master
subprocess.run(["setpci", "-s", "00:03.0", "COMMAND=0007"], check=True)

# Map BAR0
fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
bar0 = mmap.mmap(fd, 4096, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=bar0_addr)

def read_reg(off):
    bar0.seek(off)
    return struct.unpack('<I', bar0.read(4))[0]

def write_reg(off, val):
    bar0.seek(off)
    bar0.write(struct.pack('<I', val))

print(f"Device ID: 0x{read_reg(0x00):08x}")

# Enable MSI-X by setting enable bit in MSI-X cap
# MSI-X cap is at offset 0x40:
#   +0: Cap ID (0x11)
#   +1: Next cap ptr
#   +2-3: Message Control - bit 15 = MSI-X Enable
print("\nEnabling MSI-X via config space...")
result = subprocess.run(["setpci", "-s", "00:03.0", "42.w"], capture_output=True, text=True)
print(f"MSI-X control before: {result.stdout.strip()}")
# Set bit 15 (0x8000) = Enable
subprocess.run(["setpci", "-s", "00:03.0", "42.w=8003"], check=True)  # Enable + count-1
result = subprocess.run(["setpci", "-s", "00:03.0", "42.w"], capture_output=True, text=True)
print(f"MSI-X control after: {result.stdout.strip()}")

# Trigger MSI-X via IRQ_RAISE register (0x24)
print("\nTriggering IRQ_RAISE register (vector 0)...")
write_reg(0x24, 0)

print("Check QEMU stderr for 'EDU-PF: Raising MSI-X' message")

bar0.close()
os.close(fd)
