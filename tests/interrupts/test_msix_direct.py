#!/usr/bin/env python3
"""
Test MSI-X by directly programming the MSI-X table via /dev/mem.
Works without VFIO to test if QEMU's MSI-X delivery works.
"""
import mmap
import os
import struct
import subprocess

# Get BAR addresses
result = subprocess.run(["lspci", "-v", "-s", "00:03.0"], capture_output=True, text=True)
print(result.stdout)

bar0_addr = None
bar2_addr = None
for line in result.stdout.split('\n'):
    if 'Memory at' in line:
        addr = int(line.split('Memory at ')[1].split()[0], 16)
        if '64-bit' in line and bar0_addr is None:
            bar0_addr = addr
        elif '32-bit' in line:
            bar2_addr = addr

print(f"BAR0: 0x{bar0_addr:x}")
print(f"BAR2: 0x{bar2_addr:x}" if bar2_addr else "BAR2: not found")

# Enable bus master
subprocess.run(["setpci", "-s", "00:03.0", "COMMAND=0007"], check=True)

fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)

# Map BAR0
bar0 = mmap.mmap(fd, 4096, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=bar0_addr)

def read_bar0(off):
    bar0.seek(off)
    return struct.unpack('<I', bar0.read(4))[0]

def write_bar0(off, val):
    bar0.seek(off)
    bar0.write(struct.pack('<I', val))

print(f"\nDevice ID: 0x{read_bar0(0x00):08x}")

# Map BAR2 (MSI-X table)
if bar2_addr:
    bar2 = mmap.mmap(fd, 4096, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=bar2_addr)
    
    # Read MSI-X entry 0 before programming
    bar2.seek(0)
    data = bar2.read(16)
    addr_lo, addr_hi, msg_data, ctrl = struct.unpack('<IIII', data)
    print(f"\nMSI-X entry 0 before:")
    print(f"  addr: 0x{addr_hi:08x}{addr_lo:08x}")
    print(f"  data: 0x{msg_data:08x}")
    print(f"  ctrl: 0x{ctrl:08x} (masked={bool(ctrl & 1)})")
    
    # Program MSI-X entry 0 with test values
    # MSI-X address format: 0xFEE + destination APIC ID + special bits
    # For simplicity, use a test address that QEMU should recognize
    msi_addr_lo = 0xFEE00000  # x86 MSI address base (APIC local)
    msi_addr_hi = 0
    msi_data = 0x4020  # Test vector (some arbitrary interrupt vector)
    
    bar2.seek(0)
    bar2.write(struct.pack('<IIII', msi_addr_lo, msi_addr_hi, msi_data, 0))  # ctrl=0 (unmask)
    
    # Verify write
    bar2.seek(0)
    data = bar2.read(16)
    addr_lo, addr_hi, msg_data, ctrl = struct.unpack('<IIII', data)
    print(f"\nMSI-X entry 0 after programming:")
    print(f"  addr: 0x{addr_hi:08x}{addr_lo:08x}")
    print(f"  data: 0x{msg_data:08x}")
    print(f"  ctrl: 0x{ctrl:08x} (masked={bool(ctrl & 1)})")
    
    bar2.close()

# Enable MSI-X via config space (bit 15 of MSI-X control @ cap+2)
print("\nEnabling MSI-X...")
result = subprocess.run(["setpci", "-s", "00:03.0", "42.w"], capture_output=True, text=True)
print(f"MSI-X control before: {result.stdout.strip()}")
subprocess.run(["setpci", "-s", "00:03.0", "42.w=8003"], check=True)
result = subprocess.run(["setpci", "-s", "00:03.0", "42.w"], capture_output=True, text=True)
print(f"MSI-X control after: {result.stdout.strip()}")

# Trigger interrupt
print("\nTriggering IRQ_RAISE (vector 0)...")
write_bar0(0x24, 0)

print("Check QEMU output for MSI-X delivery message")

bar0.close()
os.close(fd)
