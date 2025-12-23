import time
import subprocess
import sys
import termios
import tty
import threading
from board import SCL, SDA
import busio
from PIL import Image, ImageDraw, ImageFont
import adafruit_ssd1306

# Create the I2C interface.
i2c = busio.I2C(SCL, SDA)

# Create the SSD1306 OLED class.
disp = adafruit_ssd1306.SSD1306_I2C(128, 32, i2c)

# Clear display.
disp.fill(0)
disp.show()

# Create blank image for drawing.
width = disp.width
height = disp.height
image = Image.new("1", (width, height))
draw = ImageDraw.Draw(image)

# Load default font.
font = ImageFont.load_default()

# Function to detect ESC key press
def detect_esc():
    fd = sys.stdin.fileno()
    old_settings = termios.tcgetattr(fd)
    try:
        tty.setraw(sys.stdin.fileno())
        while True:
            ch = sys.stdin.read(1)
            if ch == '\x1b':  # ESC key
                print("Exiting...")
                disp.fill(0)
                disp.show()
                sys.exit(0)
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)

# Start ESC detection in a separate thread
esc_thread = threading.Thread(target=detect_esc, daemon=True)
esc_thread.start()

# Optimization: Reduce the sleep time for faster updates
while True:
    # Draw black-filled box to clear the image
    draw.rectangle((0, 0, width, height), outline=0, fill=0)

    # Shell commands for system monitoring
    cmd = "hostname -I | cut -d' ' -f1"
    IP = subprocess.check_output(cmd, shell=True).decode("utf-8").strip()
    cmd = 'cut -f 1 -d " " /proc/loadavg'
    CPU = subprocess.check_output(cmd, shell=True).decode("utf-8").strip()
    cmd = "free -m | awk 'NR==2{printf \"Mem: %s/%s MB  %.2f%%\", $3,$2,$3*100/$2 }'"
    MemUsage = subprocess.check_output(cmd, shell=True).decode("utf-8").strip()
    cmd = 'df -h | awk \'$NF=="/"{printf "Disk: %d/%d GB  %s", $3,$2,$5}\''
    Disk = subprocess.check_output(cmd, shell=True).decode("utf-8").strip()

    # Draw text with smaller font size
    draw.text((0, 0), f"IP: {IP}", font=font, fill=255)
    draw.text((0, 10), f"CPU load: {CPU}", font=font, fill=255)
    draw.text((0, 20), MemUsage, font=font, fill=255)
    draw.text((0, 30), Disk, font=font, fill=255)

    # Display image.
    disp.image(image)
    disp.show()

    # Reduced update time
    time.sleep(0.05)  # Faster update interval (50ms)
