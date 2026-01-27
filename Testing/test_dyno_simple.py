#!/usr/bin/env python3
"""
test_dyno_simple.py
Simplified version of Dyno_Final.py to diagnose why it's not receiving data

This strips out the Dash UI and just focuses on serial reception
"""

import sys
import json
import glob
import time

print("Starting test_dyno_simple.py...")
print("Step 1: Checking imports...")

try:
    import serial
    print("  ✓ pyserial imported")
except ImportError as e:
    print(f"  ✗ pyserial import failed: {e}")
    print("  Install with: pip install pyserial")
    sys.exit(1)

print("\nStep 2: Finding serial port...")

def find_serial_port():
    """Auto-detect the RP2040 serial port"""
    patterns = [
        "/dev/serial/by-id/usb-Adafruit_Feather_RP2040_CAN_*",
        "/dev/serial/by-id/usb-Adafruit_Feather_RP2040_*",
        "/dev/ttyACM*",
    ]
    
    for pattern in patterns:
        matches = sorted(glob.glob(pattern))
        if matches:
            print(f"  ✓ Found port: {matches[0]}")
            return matches[0]
    
    print("  ✗ No port found")
    return "/dev/ttyACM0"

PORT = find_serial_port()
BAUD = 115200

print(f"\nStep 3: Opening serial port...")
print(f"  Port: {PORT}")
print(f"  Baud: {BAUD}")

try:
    ser = serial.Serial(PORT, BAUD, timeout=1)
    print("  ✓ Serial port opened!")
    ser.reset_input_buffer()
except serial.SerialException as e:
    print(f"  ✗ Failed to open: {e}")
    sys.exit(1)

print("\nStep 4: Reading data...")
print("Press Ctrl+C to exit\n")

line_count = 0
json_count = 0
ignored_count = 0
error_count = 0

try:
    while True:
        raw = ser.readline()
        
        if not raw:
            continue
        
        line = raw.decode(errors='ignore').strip()
        
        if not line:
            continue
        
        line_count += 1
        
        # Print raw line every 20 lines
        if line_count % 20 == 0:
            print(f"\n[RAW #{line_count}] {line[:80]}...")
        
        # Skip comment/status lines
        if line.startswith(("#", "DBG")) or not line.startswith("{"):
            ignored_count += 1
            if ignored_count <= 5:
                print(f"[IGNORED] {line[:60]}")
            continue
        
        # Try to parse JSON
        try:
            obj = json.loads(line)
            json_count += 1
            
            # Print every 10th packet
            if json_count % 10 == 0:
                rpm = obj.get('rpm', 0)
                tps = obj.get('tps_pct', 0)
                pkt = obj.get('pkt', 0)
                print(f"[JSON #{json_count:4d}] pkt={pkt:4d}  rpm={rpm:4d}  tps={tps:5.1f}%")
        
        except json.JSONDecodeError as e:
            error_count += 1
            print(f"\n[JSON ERROR #{error_count}]")
            print(f"  Error: {e}")
            print(f"  Line length: {len(line)} chars")
            print(f"  First 100 chars: {line[:100]}")
            print()

except KeyboardInterrupt:
    print("\n\n" + "=" * 70)
    print("FINAL STATISTICS:")
    print("=" * 70)
    print(f"Total lines:     {line_count}")
    print(f"Ignored lines:   {ignored_count}")
    print(f"Valid JSON:      {json_count}")
    print(f"JSON errors:     {error_count}")
    print(f"Success rate:    {json_count / max(1, line_count) * 100:.1f}%")
    print("=" * 70)

finally:
    ser.close()
    print("Serial port closed.")
