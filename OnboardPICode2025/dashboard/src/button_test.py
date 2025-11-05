#!/usr/bin/env python3
import RPi.GPIO as GPIO
import pygame
import time
import sys

# Use GPIO23 for Next and GPIO22 for Previous
UP_BUTTON = 23
DOWN_BUTTON = 22

# List of page names
pages = ["Drivers Page", "Diagnostics Page", "Graph Page"]
current_page = 0

# Function to update the display with the current page name
def show_page():
    screen.fill((0, 0, 0))  # Clear screen (black)
    text = font.render(pages[current_page], True, (255, 255, 255))
    text_rect = text.get_rect(center=(320, 240))
    screen.blit(text, text_rect)
    pygame.display.flip()

# Set up GPIO pins
GPIO.setmode(GPIO.BCM)
GPIO.setup(UP_BUTTON, GPIO.IN, pull_up_down=GPIO.PUD_UP)
GPIO.setup(DOWN_BUTTON, GPIO.IN, pull_up_down=GPIO.PUD_UP)

# Set up Pygame window
pygame.init()
screen = pygame.display.set_mode((640, 480))
pygame.display.set_caption("Wazzzzaaaaaaaa")
font = pygame.font.Font(None, 74)

# Show the initial page
show_page()

# Variables to hold the last state for simple edge detection
last_up = GPIO.input(UP_BUTTON)
last_down = GPIO.input(DOWN_BUTTON)

# Main loop
while True:
    # Handle Pygame events (e.g. closing the window)
    for event in pygame.event.get():
        if event.type == pygame.QUIT:
            GPIO.cleanup()
            pygame.quit()
            sys.exit()

    # Read the current state of the buttons
    current_up = GPIO.input(UP_BUTTON)
    current_down = GPIO.input(DOWN_BUTTON)

    # Check if the Next button was pressed (state goes from HIGH to LOW)
    if last_up == 1 and current_up == 0:
        current_page = (current_page + 1) % len(pages)
        print("Up button pressed!")
        show_page()
        # Wait until button is released
        while GPIO.input(UP_BUTTON) == 0:
            time.sleep(0.05)

    # Check if the Previous button was pressed (state goes from HIGH to LOW)
    if last_down == 1 and current_down == 0:
        current_page = (current_page - 1) % len(pages)
        print("Down button pressed!")
        show_page()
        # Wait until button is released
        while GPIO.input(DOWN_BUTTON) == 0:
            time.sleep(0.05)

    # Save the current page
    last_up = current_up
    last_down = current_down
