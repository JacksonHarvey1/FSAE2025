import time
import board
import neopixel
import digitalio
import sys

# ----------------------------
# NeoPixel Configuration
# ----------------------------
# Use GPIO18 (physical pin 12) for the NeoPixel data output.
PIXEL_PIN = board.D18  
NUM_PIXELS = 4  # Pixel 0: Battery, 1: Oil, 2: Engine Coolant, 3: Neutral

# For many LED strips the default color ordering is GRB.
# If the colors seem off, try changing to neopixel.RGB.
pixels = neopixel.NeoPixel(PIXEL_PIN, NUM_PIXELS, brightness=0.5, auto_write=False, pixel_order=neopixel.GRB)

# ----------------------------
# Neutral Sensor Configuration
# ----------------------------
# Use GPIO23 (physical pin 16) for the neutral signal.
NEUTRAL_SENSOR_PIN = board.D23
neutral_sensor = digitalio.DigitalInOut(NEUTRAL_SENSOR_PIN)
neutral_sensor.direction = digitalio.Direction.INPUT
neutral_sensor.pull = digitalio.Pull.DOWN  # Assumes sensor outputs HIGH when neutral is active

# ----------------------------
# Function to update NeoPixels based on sensor values
# ----------------------------
def update_neopixels(battery_voltage, oil_pressure, engine_coolant, neutral_active):
    # Pixel 0: Battery Voltage
    # If battery voltage is 12.0V or above, show green.
    if battery_voltage >= 12.0:
        pixels[0] = (0, 255, 0)      # Green
    elif battery_voltage >= 11.8:
        pixels[0] = (255, 172, 28)   # Amber
    else:
        pixels[0] = (255, 0, 0)      # Red

    # Pixel 1: Oil Pressure (PSI)
    if oil_pressure >= 20:
        pixels[1] = (0, 255, 0)      # Green
    elif oil_pressure >= 10:
        pixels[1] = (255, 172, 28)   # Amber
    else:
        pixels[1] = (255, 0, 0)      # Red

    # Pixel 2: Engine Coolant
    if engine_coolant > 200:
        pixels[2] = (255, 0, 0)      # Red
    elif engine_coolant >= 150:
        pixels[2] = (0, 255, 0)      # Green
    else:
        pixels[2] = (0, 0, 255)      # Blue

    # Pixel 3: Neutral Indicator
    # For testing, you can override the sensor reading by setting neutral_active = True
    if neutral_active:
        pixels[3] = (255, 255, 0)    # Yellow indicates neutral is active
    else:
        pixels[3] = (0, 0, 0)        # Off

    pixels.show()

# ----------------------------
# Main Loop with Ctrl+C handling and debug output
# ----------------------------
if __name__ == "__main__":
    try:
        while True:
            # Simulated sensor values; replace these with your actual readings.
            battery_voltage = 12.0  # Example: voltage in volts (set to 12.0, so Pixel 0 should be green)
            oil_pressure = 15.0     # Example: oil pressure in PSI (between 10 and 20, so Pixel 1 will be amber)
            engine_coolant = 180.0  # Example: coolant reading (between 150 and 200, so Pixel 2 will be green)

            # Read the neutral sensor value.
            # If nothing is connected, the sensor will read LOW.
            neutral_active = neutral_sensor.value

            # Uncomment the following line for testing to force the neutral indicator on:
            # neutral_active = True

            # Debug prints to monitor sensor values and the neutral sensor state.
            print(f"Battery Voltage: {battery_voltage} V | Oil Pressure: {oil_pressure} PSI | Engine Coolant: {engine_coolant} | Neutral: {neutral_active}")

            # Update the NeoPixels with the current sensor readings.
            update_neopixels(battery_voltage, oil_pressure, engine_coolant, neutral_active)

            # Delay before the next update.
            time.sleep(1)

    except KeyboardInterrupt:
        # Clear the NeoPixels upon termination with Ctrl+C.
        print("Ctrl+C detected; terminating program and clearing NeoPixels.")
        pixels.fill((0, 0, 0))
        pixels.show()
        sys.exit(0)
