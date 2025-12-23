#Duncan Keller 2/21/24
#Grabs LoRa Data From RP2040 Running "CAN Sniffer"
#This code handles receiving LoRa data from the RP2040 on the car
#The data is then unpacked and sent to a MariaDB for use on Grafana

import mysql.connector
import time
import busio
from digitalio import DigitalInOut, Direction, Pull
import board
import adafruit_ssd1306
import adafruit_rfm9x
from datetime import datetime
import pytz
import shutil

# Button A
btnA = DigitalInOut(board.D5)
btnA.direction = Direction.INPUT
btnA.pull = Pull.UP

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
rfm9x.tx_power = 23
rfm9x.spreading_factor = 8
rfm9x.receive_timeout = 1.0
rfm9x.coding_rate = 8
rfm9x.signal_bandwidth = 250000
ID = 0x22 #Header ID or RP240

#Setup Variables
count = 0       #Count of Packets
packet = None   #Raw LoRa Packet
data = []     #Converted LoRa Data

#Reset Display
display.fill(0)
display.show()

config = {
    'user': 'fsae2024',
    'password': 'YCPfsae#123',
    'host': '127.0.0.1',
    'database': 'Sensor_Data',
}

#Create Datetime if data is aved before needing a timestamp
dateTime = datetime.now()
UTCdateTime = pytz.timezone('UTC')
dateTime = dateTime.astimezone(UTCdateTime)
dateTime = dateTime.strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]

while True:
    # check for packet rx returned in bytes
    packet = rfm9x.receive(with_header=True)
    
    if not btnA.value:                          
        #Connection for MariaDB
        try:
            #connect with credentials
            conn = mysql.connector.connect(**config)
        
            cursor = conn.cursor()   #Create cursor object
            data_tuple=tuple(data)   #Format data
        
            #Ececute SQL command
            cursor.execute("INSERT INTO liveData VALUES " + str(data_tuple))
            
            conn.commit()  # Commit changes to databse
            
            #Reset Table
            if not btnA.value: 
                cursor.execute("TRUNCATE TABLE liveData")
            
            conn.close()   # Close database conncetion
            
            #Display Cleared message and 
            display.fill(0)
            display.text("Database Cleared", 15, 10, 1)
            display.show()
            time.sleep(3)
            display.fill(0)
            display.show()
        
        #Catch connection error and print
        except mysql.connector.Error as e:
            print(f"Error connecting to database: {e}")
  
    if packet is not None and packet[1] == ID:
        
    #Check if Packet has RP2040 ID
    
        # Display the packet number & RSSI
        display.fill(0)
        count += 1
        packetNum = "Packet Number: " + str(count) 
        rssi = "Last RSSI: " + str(rfm9x.rssi)
        display.text(packetNum, 15, 10, 1)
        display.text(rssi,15,20,1)
        display.show()
        
        #Print Part of Packet To Terminal for Debugging
        for x in range(4,len(packet),2):
            val = packet[x] | (packet[x+1] << 8)
        
            #NOTE this is assuming all values are signed values or
            #That the unsigned dont go higher than 32766
            if val > 32767:
                val = val - 65536
            
            #TPS, Fuel Time, Air temp, coolant temp    
            if (x==6) or (x==8) or (x==36) or (x==38):
                val = val / 10
            #Barometer, MAP, Battery Voltage, Accelerometer
            elif (x>=10 and x <=12) or (x== 34) or (x>= 40 and x<=44):
                val = val / 100
            #Freq / Wheel speed
            elif(x>= 26 and x <= 32):
                val = val / 20
            #Analog    
            elif (x>= 14 and x<= 24):
                val = val /1000 #resolution conversion

            #Append new Value onto final list
            data.append(val)
            
        #Create UTC timestamp for Grafanna
        dateTime = datetime.now()
        UTCdateTime = pytz.timezone('UTC')
        dateTime = dateTime.astimezone(UTCdateTime)
        dateTime = dateTime.strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]
        data.append(dateTime)
        print(data)
        
        try:
            #connect with credentials
            conn = mysql.connector.connect(**config)
        
            cursor = conn.cursor()   #Create cursor object
            data_tuple=tuple(data)   #Format data
        
            #Ececute SQL command
            cursor.execute("INSERT INTO liveData VALUES " + str(data_tuple))
            
            conn.commit()  # Commit changes to databse
        #Catch connection error and print
        except mysql.connector.Error as e:
            print(f"Error connecting to database: {e}")
            
        data = [] #Clear Data
    
