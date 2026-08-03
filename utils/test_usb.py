#!/usr/bin/env python3
"""Test usb-edu device via /dev/bus/usb"""
import os
import struct

# Find our device (1234:ed00)
def find_device():
    for bus in os.listdir("/sys/bus/usb/devices"):
        path = f"/sys/bus/usb/devices/{bus}"
        try:
            vid = open(f"{path}/idVendor").read().strip()
            pid = open(f"{path}/idProduct").read().strip()
            if vid == "1234" and pid == "ed00":
                busnum = int(open(f"{path}/busnum").read().strip())
                devnum = int(open(f"{path}/devnum").read().strip())
                return busnum, devnum
        except:
            pass
    return None, None

busnum, devnum = find_device()
if not busnum:
    print("Device not found!")
    exit(1)

print(f"Found usb-edu at bus {busnum}, device {devnum}")
print(f"Device path: /dev/bus/usb/{busnum:03d}/{devnum:03d}")

# To actually send USB bulk transfers, we'd need libusb or pyusb
# For now, just confirm the device exists
print("\nDevice info from sysfs:")
path = f"/sys/bus/usb/devices/{busnum}-1"
print(f"  Manufacturer: {open(f'{path}/manufacturer').read().strip()}")
print(f"  Product: {open(f'{path}/product').read().strip()}")
print(f"  Serial: {open(f'{path}/serial').read().strip()}")
