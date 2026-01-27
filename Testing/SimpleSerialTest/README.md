# Simple Serial Communication Test

This is a minimal test to verify USB serial communication between the RP2040 and Raspberry Pi.

## Purpose

Use these test files when you need to verify that basic serial communication is working before troubleshooting more complex applications like the Dyno telemetry system.

## Files

- **`SimpleSerialTest.ino`** - Arduino sketch that sends "HI {counter}" every second
- **`../simple_serial_test.py`** - Python script that reads and displays serial data

## Quick Start

### 1. Upload the Arduino Sketch

1. Open `SimpleSerialTest.ino` in Arduino IDE
2. Select **Tools → Board → Raspberry Pi RP2040 Boards → Adafruit Feather RP2040**
3. Select **Tools → Port** → (your RP2040 port)
4. Click **Upload**

### 2. Verify the RP2040 is Running

After upload, the onboard LED should blink once per second. This confirms the sketch is running even if serial communication isn't working yet.

### 3. Test with `cat` (Optional but Recommended)

On the Raspberry Pi, verify data is being sent:

```bash
# Find your serial port
ls /dev/serial/by-id/

# View raw data (Ctrl+C to exit)
cat /dev/ttyACM0
```

**Expected output:**
```
# SimpleSerialTest starting...
# Adafruit Feather RP2040
# Baud: 115200
# Sending 'HI' every 1 second
# Lines starting with '#' are status messages
#
HI 1
HI 2
HI 3
HI 4
...
```

### 4. Test with Python Script

```bash
cd ~/FSAE2025/Testing

# Auto-detect serial port
python simple_serial_test.py

# Or manually specify port
python simple_serial_test.py /dev/ttyACM0

# Or specify port and baud rate
python simple_serial_test.py /dev/ttyACM0 115200
```

**Expected output:**
```
============================================================
Simple Serial Test - RP2040 → Raspberry Pi
============================================================
Port:     /dev/ttyACM0
Baud:     115200
============================================================
Press Ctrl+C to exit
============================================================

✓ Serial port opened successfully!

[17:23:45] # SimpleSerialTest starting...
[17:23:45] # Adafruit Feather RP2040
[17:23:45] # Baud: 115200
[17:23:45] # Sending 'HI' every 1 second
[17:23:45] # Lines starting with '#' are status messages
[17:23:45] #
[17:23:46] HI 1
[17:23:47] HI 2
[17:23:48] HI 3
```

## Troubleshooting

### LED Not Blinking
- **Problem:** Sketch not uploaded or not running
- **Fix:** Re-upload the sketch, verify the correct board is selected

### `cat` Shows Nothing
- **Problem:** USB cable or connection issue
- **Fix:** Try a different USB cable or USB port on the Pi

### `cat` Shows Garbage Characters
- **Problem:** Baud rate mismatch
- **Fix:** Verify both Arduino and test script use 115200 baud

### Python Script: "Permission denied"
- **Problem:** User not in `dialout` group
- **Fix:**
  ```bash
  sudo usermod -aG dialout $USER
  # Then log out and log back in
  ```

### Python Script: "Could not find serial port"
- **Problem:** Port detection failed
- **Fix:** Manually specify the port:
  ```bash
  python simple_serial_test.py /dev/ttyACM0
  ```

### `cat` Works but Python Script Fails
- **Problem:** Port blocking or buffering issue
- **Fix:** Make sure no other program (like Arduino Serial Monitor) is using the port

## What This Test Proves

✅ **If this test works:**
- USB hardware connection is good
- Serial drivers are working
- Baud rate is correct
- Permissions are set correctly
- Python serial library is installed correctly

❌ **If this test fails:**
- There's a fundamental hardware, driver, or permission issue that must be fixed before moving to more complex applications

## Next Steps

Once this test passes, you can move on to testing:
1. The full Dyno telemetry system (`Dyno_Final.py`)
2. CAN bus communication tests
3. InfluxDB ingestion scripts

## Technical Details

- **Baud Rate:** 115200
- **Data Format:** 8N1 (8 data bits, no parity, 1 stop bit)
- **Flow Control:** None
- **Message Format:** Plain text lines ending with `\n`
- **Send Rate:** 1 message per second
