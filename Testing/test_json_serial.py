#!/usr/bin/env python3
"""
test_json_serial.py
Test script to verify Python can receive and parse JSON from the RP2040

Use this with SimpleSerialTest_JSON.ino to test JSON reception
"""

import sys
import json
import glob

try:
    import serial
except ImportError:
    print("ERROR: pyserial not installed!")
    print("Install it with: pip install pyserial")
    sys.exit(1)


def find_serial_port():
    """Auto-detect the RP2040 serial port"""
    patterns = [
        "/dev/serial/by-id/usb-Adafruit_Feather_RP2040*",
        "/dev/ttyACM*",
    ]
    
    for pattern in patterns:
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[0]
    return None


def main():
    port = find_serial_port() if len(sys.argv) < 2 else sys.argv[1]
    baud = 115200
    
    if port is None:
        print("ERROR: Could not find serial port!")
        print("Specify manually: python test_json_serial.py /dev/ttyACM0")
        sys.exit(1)
    
    print("=" * 70)
    print("JSON Serial Test - Testing JSON Reception from RP2040")
    print("=" * 70)
    print(f"Port: {port}")
    print(f"Baud: {baud}")
    print("=" * 70)
    print()
    
    try:
        ser = serial.Serial(port, baud, timeout=1)
        print("✓ Serial port opened!")
        ser.reset_input_buffer()
        print()
        
    except serial.SerialException as e:
        print(f"✗ Failed to open serial port: {e}")
        if "Permission denied" in str(e):
            print("\nFix: sudo usermod -aG dialout $USER (then re-login)")
        sys.exit(1)
    
    json_count = 0
    error_count = 0
    line_count = 0
    
    print("Receiving data... (Ctrl+C to exit)")
    print()
    
    try:
        while True:
            line_bytes = ser.readline()
            
            if line_bytes:
                line = line_bytes.decode('utf-8', errors='replace').rstrip()
                line_count += 1
                
                if not line:
                    continue
                
                # Skip status/comment lines
                if line.startswith('#'):
                    print(f"[STATUS] {line}")
                    continue
                
                # Try to parse as JSON
                if line.startswith('{'):
                    try:
                        obj = json.loads(line)
                        json_count += 1
                        
                        # Print every 10th packet to avoid spam
                        if json_count % 10 == 0:
                            print(f"[{json_count:4d}] pkt={obj.get('pkt', '?'):4d}  "
                                  f"rpm={obj.get('rpm', '?'):4d}  "
                                  f"tps={obj.get('tps_pct', '?'):5.1f}%  "
                                  f"map={obj.get('map_kpa', '?'):6.2f} kPa")
                        
                    except json.JSONDecodeError as e:
                        error_count += 1
                        print(f"[JSON ERROR #{error_count}] {e}")
                        print(f"  Line ({len(line)} chars): {line[:100]}...")
                        print()
                else:
                    print(f"[NON-JSON] {line}")
    
    except KeyboardInterrupt:
        print()
        print("=" * 70)
        print(f"Statistics:")
        print(f"  Total lines:    {line_count}")
        print(f"  Valid JSON:     {json_count}")
        print(f"  JSON errors:    {error_count}")
        print(f"  Success rate:   {json_count / max(1, line_count) * 100:.1f}%")
        print("=" * 70)
    
    finally:
        if ser.is_open:
            ser.close()


if __name__ == "__main__":
    main()
