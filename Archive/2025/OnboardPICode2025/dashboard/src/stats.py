# the code is from https://learn.adafruit.com/adafruit-pioled-128x32-mini-oled-for-raspberry-pi/usage altered using ChatGPT to properly fit
# tbis is for the Adafruit PiOLED - 128x32 Mini OLED for Raspberry Pi on the Raspi 4

import time
import subprocess

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

# Get drawing object to draw on image.
draw = ImageDraw.Draw(image)

# Draw a black filled box to clear the image.
draw.rectangle((0, 0, width, height), outline=0, fill=0)

# Set padding and initial Y position for text
padding = -2
top = padding
bottom = height - padding
line_height = 8  # You can adjust this to change the vertical space between lines
y_position = top  # Start drawing from the top

# Load a smaller font (adjust the size as needed)
font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 8)

while True:
    try:
        # Clear the display by drawing a black box
        draw.rectangle((0, 0, width, height), outline=0, fill=0)

        # Fetch system information
        cmd = "hostname -I | cut -d' ' -f1"
        IP = subprocess.check_output(cmd, shell=True).decode("utf-8")
        cmd = 'cut -f 1 -d " " /proc/loadavg'
        CPU = subprocess.check_output(cmd, shell=True).decode("utf-8")
        cmd = "free -m | awk 'NR==2{printf \"Mem: %s/%s MB  %.2f%%\", $3,$2,$3*100/$2 }'"
        MemUsage = subprocess.check_output(cmd, shell=True).decode("utf-8")
        cmd = 'df -h | awk \'$NF=="/"{printf "Disk: %d/%d GB  %s", $3,$2,$5}\''
        Disk = subprocess.check_output(cmd, shell=True).decode("utf-8")

        # Write text on the display
        y_position = top  # Reset the position to the top for each loop
        draw.text((0, y_position), "IP: " + IP, font=font, fill=255)
        y_position += line_height
        draw.text((0, y_position), "CPU load: " + CPU, font=font, fill=255)
        y_position += line_height
        draw.text((0, y_position), MemUsage, font=font, fill=255)
        y_position += line_height
        draw.text((0, y_position), Disk, font=font, fill=255)

        # Display image on the OLED
        disp.image(image)
        disp.show()

        # Sleep before updating again
        time.sleep(0.05)

    except Exception as e:
        print(f"Error: {e}")
        break  # Break the loop if there is an error