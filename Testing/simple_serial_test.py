#!/usr/bin/env python3
"""
simple_serial_test.py
Minimal serial reader for testing RP2040 → Raspberry Pi USB serial communication

This script reads from the RP2040 USB serial port and prints everything it receives.
Use this with SimpleSerialTest.ino to verify basic serial communication is working.

Usage:
    python simple_serial_test.py
    python simple_serial_test.py /dev/ttyACM0        # specify port manually
    python simple_serial_test.py /dev/ttyACM0 9600   # specify port and baud
"""

import sys
import time
import glob
from datetime import datetime

try:
    import serial
except ImportError:
    print("ERROR: pyserial not installed!")
    print("Install it with: pip install pyserial")
    sys.exit(1)


def find_serial_port():
    """Auto-detect the RP2040 serial port"""
    # Try to find Adafruit Feather RP2040 by ID
    patterns = [
        "/dev/serial/by-id/usb-Adafruit_Feather_RP2040*",
        "/dev/ttyACM*",
        "/dev/ttyUSB*",
    ]
    
    for pattern in patterns:
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[0]
    
    return None


def main():
    # Parse command line arguments
    if len(sys.argv) >= 2:
        port = sys.argv[1]
    else:
        port = find_serial_port()
        if port is None:
            print("ERROR: Could not find serial port!")
            print("\nTry manually specifying the port:")
            print("  python simple_serial_test.py /dev/ttyACM0")
            print("\nAvailable ports:")
            for pattern in ["/dev/ttyACM*", "/dev/ttyUSB*"]:
                matches = glob.glob(pattern)
                for match in matches:
                    print(f"  {match}")
            sys.exit(1)
    
    if len(sys.argv) >= 3:
        baud = int(sys.argv[2])
    else:
        baud = 115200
    
    print("=" * 60)
    print("Simple Serial Test - RP2040 → Raspberry Pi")
    print("=" * 60)
    print(f"Port:     {port}")
    print(f"Baud:     {baud}")
    print("=" * 60)
    print("Press Ctrl+C to exit")
    print("=" * 60)
    print()
    
    # Try to open the serial port
    try:
        ser = serial.Serial(port, baud, timeout=1)
        print(f"✓ Serial port opened successfully!")
        print()
        
        # Clear any stale data in the buffer
        ser.reset_input_buffer()
        
    except serial.SerialException as e:
        print(f"✗ Failed to open serial port: {e}")
        print()
        
        # Provide helpful error messages
        if "Permission denied" in str(e):
            print("PERMISSION ERROR:")
            print("  Your user doesn't have permission to access the serial port.")
            print("  Fix this by adding your user to the 'dialout' group:")
            print()
            print(f"    sudo usermod -aG dialout $USER")
            print()
            print("  Then log out and log back in for the change to take effect.")
        elif "No such file or directory" in str(e):
            print("PORT NOT FOUND:")
            print("  The specified serial port doesn't exist.")
            print("  Check available ports with: ls /dev/ttyACM* /dev/ttyUSB*")
        
        sys.exit(1)
    
    except Exception as e:
        print(f"✗ Unexpected error: {e}")
        sys.exit(1)
    
    # Read and print serial data
    line_count = 0
    
    try:
        while True:
            try:
                # Read one line (until \n)
                line_bytes = ser.readline()
                
                if line_bytes:
                    # Decode to string, removing trailing whitespace
                    line = line_bytes.decode('utf-8', errors='replace').rstrip()
                    
                    if line:  # Only print non-empty lines
                        timestamp = datetime.now().strftime("%H:%M:%S")
                        line_count += 1
                        
                        # Print with timestamp
                        print(f"[{timestamp}] {line}")
                        
            except UnicodeDecodeError as e:
                print(f"[DECODE ERROR] {e} - Raw bytes: {line_bytes.hex()}")
            
            except Exception as e:
                print(f"[READ ERROR] {e}")
                time.sleep(0.1)
    
    except KeyboardInterrupt:
        print()
        print("=" * 60)
        print(f"Exiting... (received {line_count} lines)")
        print("=" * 60)
    
    finally:
        if ser.is_open:
            ser.close()


if __name__ == "__main__":
    main()
