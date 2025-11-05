import kivy
from kivy.app import App
from kivy.uix.widget import Widget
from kivy.graphics import Line
from kivy.clock import Clock
import math

kivy.require('2.1.0')  # Ensure you have the correct Kivy version

class AnalogGaugeWidget(Widget):
    def __init__(self, **kwargs):
        super(AnalogGaugeWidget, self).__init__(**kwargs)
        self.angle = 0
        self.gauge_radius = 100  # Radius of the gauge
        self.center = (200, 200)  # Center point of the gauge
        self.update_gauge(0)  # Draw the initial gauge
        Clock.schedule_interval(self.update, 0.1)  # Update the gauge every 0.1 seconds

    def update(self, dt):
        # Simulate a real-time data update (increment the angle value)
        self.angle = (self.angle + 1) % 101  # Loop back to 0 after reaching 100
        self.update_gauge(self.angle)  # Update the gauge with the new angle

    def update_gauge(self, value):
        self.canvas.clear()  # Clear the previous drawing
        
        # Draw the gauge background (circle)
        with self.canvas:
            # Outer circle for gauge
            self.canvas.add(Line(circle=(self.center[0], self.center[1], self.gauge_radius), width=2))
        
        # Draw the needle (red line) based on the current value
        angle = (value / 100) * 180  # Convert value (0-100) to angle (0-180 degrees)
        radian = math.radians(180 - angle)  # Convert angle to radians
        x = self.center[0] + self.gauge_radius * math.cos(radian)
        y = self.center[1] - self.gauge_radius * math.sin(radian)

        with self.canvas:
            # Red needle pointing to the current value
            self.canvas.add(Line(points=[self.center[0], self.center[1], x, y], width=4, color=(1, 0, 0, 1)))

class AnalogGaugeApp(App):
    def build(self):
        return AnalogGaugeWidget()

if __name__ == "__main__":
    AnalogGaugeApp().run()