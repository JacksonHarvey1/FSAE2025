import time
import board
import neopixel
import sys

# ----------------------------
# NeoPixel Configuration for Test
# ----------------------------
PIXEL_PIN = board.D18   # Use GPIO18 (physical pin 12) for data output.
NUM_PIXELS = 4          # Total number of LEDs in your strip.

# Try using neopixel.RGB so that the tuple (0, 255, 0) produces pure green.
# If this does not work (if one LED still comes up yellow), try changing to neopixel.GRB or the other orders.
pixels = neopixel.NeoPixel(PIXEL_PIN, NUM_PIXELS, brightness=0.5, auto_write=False, pixel_order=neopixel.RGB)

def test_all_green(duration=5):
    """
    Sets all NeoPixels to green for the specified duration (in seconds)
    and then clears the LEDs.
    """
    print("Setting all pixels to green...")
    # Here (0, 255, 0) is intended to be pure green.
    pixels.fill((0, 255, 0))
    pixels.show()
    time.sleep(duration)
    
    print("Test complete; turning off LEDs.")
    pixels.fill((0, 0, 0))
    pixels.show()

if __name__ == "__main__":
    try:
        test_all_green(duration=5)
    except KeyboardInterrupt:
        print("Ctrl+C detected; clearing LEDs and exiting.")
        pixels.fill((0, 0, 0))
        pixels.show()
        sys.exit(0)
