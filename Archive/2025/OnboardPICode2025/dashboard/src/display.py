#!/usr/bin/env python3
"""
display.py

A Kivy-based dashboard for FSAE that displays:
  - Drivers page: Engine RPM.
  - Diagnostics page: Coolant Temp, Battery Voltage, Oil Pressure,
    Ignition Angle, Fuel Open Time, Air Temp.
  - Wheel Speed page: Wheel speeds for each wheel.
Navigation via GPIO buttons or ↑/↓ arrows. Full-screen auto.
"""

import os
# Force unexport of GPIO pins in sysfs in case they're stuck
os.system("echo 17 > /sys/class/gpio/unexport 2>/dev/null || true")
os.system("echo 27 > /sys/class/gpio/unexport 2>/dev/null || true")
import sys
import signal
import threading
import time
import can
from threading import Lock

# Force Kivy to use the standard OpenGL backend.
os.environ['KIVY_GL_BACKEND']            = 'gl'
os.environ['MESA_GL_VERSION_OVERRIDE']   = '3.0'
os.environ['MESA_GLSL_VERSION_OVERRIDE'] = '130'
os.environ['LIBGL_DRI3_DISABLE']         = '1'

# Full-screen auto
from kivy.config    import Config
Config.set('graphics', 'fullscreen', 'auto')
from kivy.core.window import Window
Window.clearcolor = (1,1,1,1)

from kivy.app               import App
from kivy.uix.label         import Label
from kivy.uix.boxlayout     import BoxLayout
from kivy.uix.gridlayout    import GridLayout
from kivy.uix.screenmanager import ScreenManager, Screen, SlideTransition
from kivy.clock             import Clock

import RPi.GPIO as GPIO
# Immediately release any pins claimed by previous runs
GPIO.cleanup()
from data_parser import parse_can_message

# Shared CAN data + lock
can_data      = {}
can_data_lock = Lock()

# GPIO pins
PAGE_UP_PIN   = 17
PAGE_DOWN_PIN = 27

# Constants
TIRE_CIRCUMFERENCE_FT = 3.4  # ft per rotation

# Bring up/down CAN interface
def network_up():
    os.system("sudo ip link set can0 down")
    os.system("sudo ip link set can0 type can bitrate 250000")
    os.system("sudo ip link set can0 up")

def network_down():
    os.system("sudo ip link set can0 down")

########################################
# 1) BorderedLabel with inner-border support
########################################
from kivy.graphics import Color, Line
class BorderedLabel(Label):
    def __init__(self, *args, inner_color=(1,1,1,1), **kwargs):
        if args:
            kwargs['text'] = args[0]
        super().__init__(**kwargs)
        self._inner_color = inner_color
        self._inner_inset = 4
        self._inner_width = 2
        self.halign = 'center'
        self.valign = 'middle'
        self.bind(size=self._update_text_size)
        with self.canvas.before:
            Color(0,0,0,1)
            self._outer_line = Line(rectangle=(self.x, self.y, self.width, self.height), width=1)
            self._inner_color_instr = Color(*self._inner_color)
            self._inner_line = Line(
                rectangle=(self.x + self._inner_inset,
                           self.y + self._inner_inset,
                           self.width - 2*self._inner_inset,
                           self.height - 2*self._inner_inset),
                width=self._inner_width
            )
        self.bind(pos=self._update_lines, size=self._update_lines)
    def _update_text_size(self, *a):
        self.text_size = (self.width, self.height)
    def _update_lines(self, *a):
        self._outer_line.rectangle = (self.x, self.y, self.width, self.height)
        i = self._inner_inset
        self._inner_line.rectangle = (self.x + i, self.y + i, self.width - 2*i, self.height - 2*i)
    def set_inner_color(self, rgba):
        self._inner_color_instr.rgba = rgba

########################################
# 2) Drivers Page
########################################
class DriversPage(Screen):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        box = BoxLayout(orientation='vertical', padding=2, spacing=2)
        self.rpm = BorderedLabel("Engine RPM: --", font_size='120sp', size_hint=(1,1), color=(0,0,0,1))
        box.add_widget(self.rpm)
        self.add_widget(box)
    def update_data(self, d):
        self.rpm.text = f"Engine RPM: {d['RPM']:.0f}" if 'RPM' in d else "Engine RPM: --"

########################################
# 3) Diagnostics Page
########################################
class DiagnosticsPage(Screen):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        grid = GridLayout(cols=2, rows=3, padding=2, spacing=2)
        self.coolant  = BorderedLabel("Coolant Temp: --", font_size='50sp', size_hint=(1,1))
        self.batt     = BorderedLabel("Battery Voltage: --", font_size='50sp', size_hint=(1,1))
        self.oil      = BorderedLabel("Oil Pressure: --", font_size='50sp', size_hint=(1,1))
        self.ignition = BorderedLabel("Ignition Angle: --", font_size='50sp', size_hint=(1,1))
        self.fuel     = BorderedLabel("Fuel Open Time: --", font_size='50sp', size_hint=(1,1))
        self.air      = BorderedLabel("Air Temp: --", font_size='50sp', size_hint=(1,1))
        for lbl in (self.coolant, self.batt, self.oil, self.ignition, self.fuel, self.air):
            lbl.color = (0,0,0,1)
            grid.add_widget(lbl)
        self.add_widget(grid)

    def update_data(self, d):
        # Oil Pressure
        if 'Oil_Pressure_PSI' in d:
            val = d['Oil_Pressure_PSI']
            self.oil.text = f"Oil Pressure: {val:.1f} PSI"
            if val >= 20:
                self.oil.set_inner_color((0,1,0,1))
            elif val >= 10:
                self.oil.set_inner_color((1,172/255,28/255,1))
            else:
                self.oil.set_inner_color((1,0,0,1))
        else:
            self.oil.text = "Oil Pressure: --"
            self.oil.set_inner_color((1,1,1,1))

        # Battery Voltage
        if 'Battery_Voltage' in d:
            val = d['Battery_Voltage']
            self.batt.text = f"Battery Voltage: {val:.2f} V"
            if val >= 12:
                self.batt.set_inner_color((0,1,0,1))
            elif val >= 11.8:
                self.batt.set_inner_color((1,172/255,28/255,1))
            else:
                self.batt.set_inner_color((1,0,0,1))
        else:
            self.batt.text = "Battery Voltage: --"
            self.batt.set_inner_color((1,1,1,1))

        # Swapped: Coolant Temp now shows original Air Temp ×10
        if 'Air_Temp' in d:
            scaled_air = d['Air_Temp'] 
            self.coolant.text = f"Coolant Temp: {scaled_air:.1f} °F"
            if scaled_air > 200:
                self.coolant.set_inner_color((1,0,0,1))
            elif scaled_air >= 150:
                self.coolant.set_inner_color((0,1,0,1))
            else:
                self.coolant.set_inner_color((0,0,1,1))
        else:
            self.coolant.text = "Coolant Temp: --"
            self.coolant.set_inner_color((1,1,1,1))

        # Ignition Angle
        self.ignition.text = f"Ignition Angle: {d['Ignition_Angle']:.1f} °" if 'Ignition_Angle' in d else "Ignition Angle: --"

        # Fuel Open Time
        self.fuel.text = f"Fuel Open Time: {d['Fuel_Open_Time']:.1f} ms" if 'Fuel_Open_Time' in d else "Fuel Open Time: --"

        # Swapped: Air Temp now shows original Coolant Temp ÷10
        if 'Engine_Coolant' in d:
            coolant_deg = d['Engine_Coolant'] 
            self.air.text = f"Air Temp: {coolant_deg:.1f} °F"
        else:
            self.air.text = "Air Temp: --"

########################################
# 4) Wheel Speed Page
########################################
class WheelSpeedPage(Screen):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        grid = GridLayout(cols=2, rows=2, padding=2, spacing=2)
        self.fl = BorderedLabel("Wheel Speed FL: --", font_size='50sp', size_hint=(1,1))
        self.fr = BorderedLabel("Wheel Speed FR: --", font_size='50sp', size_hint=(1,1))
        self.bl = BorderedLabel("Wheel Speed BL: --", font_size='50sp', size_hint=(1,1))
        self.br = BorderedLabel("Wheel Speed BR: --", font_size='50sp', size_hint=(1,1))
        for lbl in (self.fl, self.fr, self.bl, self.br):
            lbl.color = (0,0,0,1)
            grid.add_widget(lbl)
        self.add_widget(grid)

    def update_data(self, d):
        def F(k,name):
            if k in d:
                hz = d[k]
                mph = hz * TIRE_CIRCUMFERENCE_FT * 0.6818
                return f"{name}: {hz:.1f}Hz ({mph:.1f}MPH)"
            return f"{name}: --"
        self.fl.text = F('WheelSpeed_FL_Hz', "Wheel Speed FL")
        self.fr.text = F('WheelSpeed_FR_Hz', "Wheel Speed FR")
        self.bl.text = F('WheelSpeed_BL_Hz', "Wheel Speed BL")
        self.br.text = F('WheelSpeed_BR_Hz', "Wheel Speed BR")

########################################
# 5) CAN thread & navigation callbacks
########################################
def can_reading_thread():
    try:
        bus = can.interface.Bus(channel='can0', bustype='socketcan')
    except Exception as e:
        print("CAN open error:", e)
        return
    while True:
        msg = bus.recv(timeout=1.0)
        if msg:
            new = parse_can_message(msg.arbitration_id, msg.data)
            with can_data_lock:
                can_data.update(new)
        time.sleep(0.01)

current_screen = 0

def page_up(ch=None):
    global current_screen, screen_mgr
    current_screen = (current_screen - 1) % len(screen_mgr.screen_names)
    Clock.schedule_once(lambda dt: screen_mgr.transition_to(screen_mgr.screen_names[current_screen], direction='right'))

def page_down(ch=None):
    global current_screen, screen_mgr
    current_screen = (current_screen + 1) % len(screen_mgr.screen_names)
    Clock.schedule_once(lambda dt: screen_mgr.transition_to(screen_mgr.screen_names[current_screen], direction='left'))

class MyScreenMgr(ScreenManager):
    def transition_to(self, name, direction='left'):
        self.transition = SlideTransition(direction=direction)
        self.current = name

########################################
# 6) Main App & graceful shutdown
########################################
class DashboardApp(App):
    def build(self):
        Window.fullscreen = 'auto'
        global screen_mgr
        screen_mgr = MyScreenMgr()
        screen_mgr.add_widget(DriversPage(name="drivers"))
        screen_mgr.add_widget(DiagnosticsPage(name="diagnostics"))
        screen_mgr.add_widget(WheelSpeedPage(name="wheels"))
        Window.bind(on_key_down=self._on_key_down)
        Clock.schedule_interval(self._refresh, 0.5)
        return screen_mgr

    def _refresh(self, dt):
        with can_data_lock:
            snap = can_data.copy()
        for nm in self.root.screen_names:
            self.root.get_screen(nm).update_data(snap)

    def _on_key_down(self, w, key, *args):
        if key == 273: page_up(); return True
        if key == 274: page_down(); return True
        return False

if __name__ == "__main__":
    GPIO.cleanup()               # clear stale pin claims
    network_up()
    threading.Thread(target=can_reading_thread, daemon=True).start()

    # GPIO setup (inside main to ensure cleanup on exit)
    GPIO.setmode(GPIO.BCM)
    GPIO.setup(PAGE_UP_PIN,   GPIO.IN, pull_up_down=GPIO.PUD_UP)
    GPIO.setup(PAGE_DOWN_PIN, GPIO.IN, pull_up_down=GPIO.PUD_UP)
    GPIO.add_event_detect(PAGE_UP_PIN,   GPIO.FALLING, callback=page_up,   bouncetime=300)
    GPIO.add_event_detect(PAGE_DOWN_PIN, GPIO.FALLING, callback=page_down, bouncetime=300)

    try:
        DashboardApp().run()
    finally:
        GPIO.cleanup()
        network_down()
