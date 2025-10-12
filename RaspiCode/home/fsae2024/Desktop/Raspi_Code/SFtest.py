import csv
import time
import busio
from digitalio import DigitalInOut, Direction, Pull
import board
import adafruit_ssd1306
import adafruit_rfm9x
from datetime import datetime

# Button A
btnA = DigitalInOut(board.D5)
btnA.direction = Direction.INPUT
btnA.pull = Pull.UP

# Button B
btnB = DigitalInOut(board.D6)
btnB.direction = Direction.INPUT
btnB.pull = Pull.UP

# Button C
btnC = DigitalInOut(board.D12)
btnC.direction = Direction.INPUT
btnC.pull = Pull.UP

# Create the I2C interface.
i2c = busio.I2C(board.SCL, board.SDA)

# 128x32 OLED Display
reset_pin = DigitalInOut(board.D4)
display = adafruit_ssd1306.SSD1306_I2C(128, 32, i2c, reset=reset_pin)

# Clear the display.
display.fill(0)
display.show()
width = display.width
height = display.height

# Configure LoRa Radio
CS = DigitalInOut(board.CE1)
RESET = DigitalInOut(board.D25)
spi = busio.SPI(board.SCK, MOSI=board.MOSI, MISO=board.MISO)
rfm9x = adafruit_rfm9x.RFM9x(spi, CS, RESET, 915.0)

def myFunction():
        #Setup Variables
        count = 0       #Count of Packets
        packet = None   #Raw LoRa Packet
        data = []     # Final Data to write via csv

        #Start Timer for elapsed time
        startTime = time.time()

        #Get datetime
        # c
        dateTime = datetime.now().strftime("%m/%d/%Y__%H/%M/%S")

        stime = time.time()

        while rfm9x.spreading_factor >= 7:
            # check for packet rx returned in bytes
            packet = rfm9x.receive(with_header=True)
            
            #if no packets for 30s then end and log data
            if (time.time() - stime) >= 15:
                 break
            
            
            if packet is None:
                display.show()
                display.fill(0)
                packetNum = "Packet Number: " + str(count) 
                rssi = "Last RSSI: " + str(rfm9x.rssi)
                display.text(packetNum, 15, 10, 1)
                display.text(rssi,15,20,1)
                
            
            else:
                # Display the packet number & RSSI
                display.fill(0)
                count += 1
                packetNum = "Packet Number: " + str(count) 
                rssi = "Last RSSI: " + str(rfm9x.rssi)
                display.text(packetNum, 15, 10, 1)
                display.text(rssi,15,20,1)
                display.show()
                
                packet = [int(byte) for byte in packet]
                
                packet.append(rfm9x.rssi)
                packet.append(rfm9x.snr)
                packet.append(rfm9x.coding_rate)
                packet.append(round(time.time() - stime,2))
                print(packet)
                data.append(packet)
                stime = time.time()
                
        #Make CSV File to save data to
        csv_file = open("data.csv", 'a', newline='')
        writer = csv.writer(csv_file)
        
        #write data to csv and close file
        for row in data:
            writer.writerow(row)
        csv_file.close()
        print("done")

        display.fill(0)
        packetNum = "DONE"
        display.text(packetNum, 15, 10, 1)
        display.show()


#allow buttons to change bandwidth setting 
while(1):
        display.show()
        display.fill(0)
        display.text('start',15,20,1)
        if not btnA.value:
                time.sleep(2)
                rfm9x.spreading_factor = 9
                rfm9x.signal_bandwidth = 500000
                rfm9x.coding_rate = 8
                myFunction()
        elif not btnB.value:
                time.sleep(2)
                rfm9x.spreading_factor = 8
                rfm9x.signal_bandwidth = 250000
                rfm9x.coding_rate = 8
                myFunction()
        elif not btnC.value:
                time.sleep(2)
                rfm9x.spreading_factor = 7
                rfm9x.signal_bandwidth = 125000
                rfm9x.coding_rate = 8
                myFunction()
