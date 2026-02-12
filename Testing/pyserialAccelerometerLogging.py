import csv
import time
import serial

PORT = "COM5"       # <-- change this (Windows: COMx, Mac/Linux: /dev/ttyACM0 or /dev/tty.usbmodem...)
BAUD = 115200

out_name = time.strftime("imu_log_%Y%m%d_%H%M%S.csv")
ser = serial.Serial(PORT, BAUD, timeout=1)

print(f"Logging from {PORT} to {out_name} ... Ctrl+C to stop")

with open(out_name, "w", newline="") as f:
    writer = None

    while True:
        line = ser.readline().decode(errors="ignore").strip()
        if not line:
            continue

        # Skip comments except the header line you print after calibration
        if line.startswith("#"):
            # When your code prints: "# Streaming: ts_ms,ax_g,..."
            if "Streaming:" in line:
                header = line.split("Streaming:", 1)[1].strip()
                cols = [c.strip() for c in header.split(",")]
                writer = csv.writer(f)
                writer.writerow(cols)
                f.flush()
                print("Header written:", cols)
            continue

        # Data lines: ts_ms,ax_g,ay_g,...
        if writer is not None:
            writer.writerow(line.split(","))
            # flush occasionally so you don't lose much if you stop it
            if int(time.time()) % 2 == 0:
                f.flush()
