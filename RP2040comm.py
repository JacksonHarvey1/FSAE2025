import serial, time, sys

PORT = "/dev/ttyACM0"   # may be /dev/ttyACM1; check `ls /dev/ttyACM*`
BAUD = 115200

ser = serial.Serial(PORT, BAUD, timeout=1)
time.sleep(1.0)

# Example: send a command to the RP2040
ser.write(b"PING\n")

print("Reading from", PORT)
while True:
    line = ser.readline().decode(errors="ignore").strip()
    if not line:
        continue

    print(line)

    # Example: if you want to react to something
    if line == "PONG":
        ser.write(b"LED ON\n")
