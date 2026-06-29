#!/bin/bash
# P2P DMA test using /dev/mem
# Device A writes to Device B's scratch register

# Get BAR0 addresses from lspci
BAR_A=$(setpci -s 00:04.0 BASE_ADDRESS_0)
BAR_B=$(setpci -s 00:05.0 BASE_ADDRESS_0)

# Strip the bottom bits (type indicator)
BAR_A=$((0x${BAR_A} & ~0xF))
BAR_B=$((0x${BAR_B} & ~0xF))

echo "Device A BAR0: 0x$(printf '%x' $BAR_A)"
echo "Device B BAR0: 0x$(printf '%x' $BAR_B)"

# Calculate Device B's scratch register address (BAR + 0x04)
TARGET_ADDR=$((BAR_B + 0x04))
echo "Target addr (B's scratch): 0x$(printf '%x' $TARGET_ADDR)"

# Read Device B's scratch before
echo ""
echo "=== Before P2P ==="
devmem2 $((BAR_B + 0x04)) w  # Read B's scratch

# Program Device A for P2P DMA
echo ""
echo "=== Triggering P2P DMA ==="
devmem2 $((BAR_A + 0x2C)) w $TARGET_ADDR  # REG_P2P_ADDR = B's scratch addr
devmem2 $((BAR_A + 0x30)) w 0xCAFEBABE    # REG_P2P_DATA = data to write
devmem2 $((BAR_A + 0x34)) w 1             # REG_P2P_CMD = trigger!

# Read Device B's scratch after
echo ""
echo "=== After P2P ==="
devmem2 $((BAR_B + 0x04)) w  # Read B's scratch

echo ""
echo "If B's scratch is 0xCAFEBABE, P2P worked!"
