# Import necessary Kivy modules for the UI and scheduling updates
from kivy.app import App
from kivy.uix.screenmanager import ScreenManager, Screen, SlideTransition
from kivy.uix.label import Label
from kivy.uix.boxlayout import BoxLayout
from kivy.uix.gridlayout import GridLayout
from kivy.clock import Clock

# Import random to simulate data values
import random

# Try to import RPi.GPIO for handling physical push buttons.
# If not available (e.g., when testing on non-Raspberry Pi systems), GPIO will be None.
try:
    import RPi.GPIO as GPIO
except ImportError:
    GPIO = None

# -----------------------------------------------------------------------------
# Define the Drivers Page: displays RPM, MPH, and Gear indicator.
# -----------------------------------------------------------------------------
class DriversPage(Screen):
    def __init__(self, **kwargs):
        super(DriversPage, self).__init__(**kwargs)
        # Use a vertical BoxLayout for stacking labels.
        layout = BoxLayout(orientation='vertical')
        # Create labels with a large font size for easy readability.
        self.rpm_label = Label(text="RPM: 0", font_size='48sp')
        self.mph_label = Label(text="MPH: 0", font_size='48sp')
        self.gear_label = Label(text="Gear: 0", font_size='48sp')
        # Add labels to the layout.
        layout.add_widget(self.rpm_label)
        layout.add_widget(self.mph_label)
        layout.add_widget(self.gear_label)
        self.add_widget(layout)

# -----------------------------------------------------------------------------
# Define the Diagnostics Page: displays Oil Pressure, Coolant Temp,
# Air/Fuel Ratio, and Battery Voltage evenly in a grid layout.
# -----------------------------------------------------------------------------
class DiagnosticsPage(Screen):
    def __init__(self, **kwargs):
        super(DiagnosticsPage, self).__init__(**kwargs)
        # Use a GridLayout with 2 columns to display the four parameters.
        layout = GridLayout(cols=2)
        self.oil_label = Label(text="Oil Pressure: 0 psi", font_size='32sp')
        self.coolant_label = Label(text="Engine Coolant Temp: 0 °F", font_size='32sp')
        self.afr_label = Label(text="Air/Fuel Ratio: 0", font_size='32sp')
        self.battery_label = Label(text="Battery Voltage: 0 V", font_size='32sp')
        # Add labels to the layout.
        layout.add_widget(self.oil_label)
        layout.add_widget(self.coolant_label)
        layout.add_widget(self.afr_label)
        layout.add_widget(self.battery_label)
        self.add_widget(layout)

# -----------------------------------------------------------------------------
# Main Application Class: sets up the ScreenManager, simulates data updates,
# and handles navigation via physical push buttons.
# -----------------------------------------------------------------------------
class FSAEDashboardApp(App):
    def build(self):
        # Create the ScreenManager with a sliding transition between pages.
        self.sm = ScreenManager(transition=SlideTransition())
        self.pages = []

        # Create and add the Drivers page.
        self.drivers_page = DriversPage(name="drivers")
        self.sm.add_widget(self.drivers_page)
        self.pages.append("drivers")

        # Create and add the Diagnostics page.
        self.diagnostics_page = DiagnosticsPage(name="diagnostics")
        self.sm.add_widget(self.diagnostics_page)
        self.pages.append("diagnostics")

        # Set the starting page index.
        self.current_page_index = 0

        # Schedule the update_data method to run repeatedly (every 0.1 seconds).
        Clock.schedule_interval(self.update_data, 0.1)

        # Setup the physical push buttons for scrolling through pages.
        self.setup_buttons()

        return self.sm

    def update_data(self, dt):
        # Simulate random values for the Drivers page.
        rpm = random.randint(800, 8000)   # RPM between 800 and 8000.
        mph = random.randint(0, 200)        # Speed between 0 and 200 MPH.
        gear = random.randint(1, 6)         # Gear between 1 and 6.
        self.drivers_page.rpm_label.text = f"RPM: {rpm}"
        self.drivers_page.mph_label.text = f"MPH: {mph}"
        self.drivers_page.gear_label.text = f"Gear: {gear}"

        # Simulate random values for the Diagnostics page.
        oil_pressure = round(random.uniform(20, 80), 1)  # Oil Pressure (psi).
        coolant_temp = round(random.uniform(180, 250), 1)  # Coolant Temp (°F).
        afr = round(random.uniform(10, 20), 2)             # Air/Fuel Ratio.
        battery_voltage = round(random.uniform(12, 14), 2) # Battery Voltage (V).
        self.diagnostics_page.oil_label.text = f"Oil Pressure: {oil_pressure} psi"
        self.diagnostics_page.coolant_label.text = f"Engine Coolant Temp: {coolant_temp} °F"
        self.diagnostics_page.afr_label.text = f"Air/Fuel Ratio: {afr}"
        self.diagnostics_page.battery_label.text = f"Battery Voltage: {battery_voltage} V"

    def setup_buttons(self):
        # Set up the physical push buttons if GPIO is available.
        if GPIO:
            GPIO.setmode(GPIO.BCM)
            # Define GPIO pins for the "next" and "previous" buttons.
            # Next button remains on GPIO 17.
            self.next_button_pin = 17
            # Change previous button to GPIO 22 (which is not a power pin).
            self.prev_button_pin = 22

            # Set the pins as inputs with pull-up resistors.
            GPIO.setup(self.next_button_pin, GPIO.IN, pull_up_down=GPIO.PUD_UP)
            GPIO.setup(self.prev_button_pin, GPIO.IN, pull_up_down=GPIO.PUD_UP)

            # Add event detection for button presses.
            GPIO.add_event_detect(self.next_button_pin, GPIO.FALLING, callback=self.next_page_callback, bouncetime=300)
            GPIO.add_event_detect(self.prev_button_pin, GPIO.FALLING, callback=self.prev_page_callback, bouncetime=300)
        else:
            print("GPIO library not available. Physical buttons will not work.")

    # Callback function when the "next" button is pressed.
    def next_page_callback(self, channel):
        print("Next button pressed!")
        Clock.schedule_once(lambda dt: self.next_page(), 0)

    # Callback function when the "previous" button is pressed.
    def prev_page_callback(self, channel):
        print("Previous button pressed!")
        Clock.schedule_once(lambda dt: self.prev_page(), 0)

    def next_page(self):
        # Move to the next page (wrap around if at the end).
        self.current_page_index = (self.current_page_index + 1) % len(self.pages)
        self.sm.current = self.pages[self.current_page_index]
        print("Switched to page:", self.sm.current)

    def prev_page(self):
        # Move to the previous page (wrap around if at the beginning).
        self.current_page_index = (self.current_page_index - 1) % len(self.pages)
        self.sm.current = self.pages[self.current_page_index]
        print("Switched to page:", self.sm.current)

    def on_stop(self):
        # Clean up the GPIO settings when the app stops.
        if GPIO:
            GPIO.cleanup()

# -----------------------------------------------------------------------------
# Main entry point: run the app.
# -----------------------------------------------------------------------------
if __name__ == '__main__':
    FSAEDashboardApp().run()
