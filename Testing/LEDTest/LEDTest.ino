// LED Test - SK6812 addressable LEDs on D6, D7, D9, D10, D11
// Sweeps one at a time starting from D6
// Requires: Adafruit NeoPixel library

#include <Adafruit_NeoPixel.h>

#define LEDS_PER_PIN 1

const int LED_PINS[]  = {6, 7, 9, 10, 11};
const int NUM_STRANDS = 5;

Adafruit_NeoPixel strands[NUM_STRANDS] = {
  Adafruit_NeoPixel(LEDS_PER_PIN, 6,  NEO_GRB + NEO_KHZ800),
  Adafruit_NeoPixel(LEDS_PER_PIN, 7,  NEO_GRB + NEO_KHZ800),
  Adafruit_NeoPixel(LEDS_PER_PIN, 9,  NEO_GRB + NEO_KHZ800),
  Adafruit_NeoPixel(LEDS_PER_PIN, 10, NEO_GRB + NEO_KHZ800),
  Adafruit_NeoPixel(LEDS_PER_PIN, 11, NEO_GRB + NEO_KHZ800),
};

const uint32_t RED   = Adafruit_NeoPixel::Color(255, 0,   0);
const uint32_t GREEN = Adafruit_NeoPixel::Color(0,   255, 0);
const uint32_t BLUE  = Adafruit_NeoPixel::Color(0,   0,   255);
const uint32_t WHITE = Adafruit_NeoPixel::Color(255, 255, 255);
const uint32_t OFF   = Adafruit_NeoPixel::Color(0,   0,   0);

void setAll(uint32_t color) {
  for (int i = 0; i < NUM_STRANDS; i++) {
    strands[i].fill(color);
    strands[i].show();
  }
}

void setup() {
  Serial.begin(115200);
  for (int i = 0; i < NUM_STRANDS; i++) {
    strands[i].begin();
    strands[i].setBrightness(128);
    strands[i].clear();
    strands[i].show();
  }
  Serial.println("SK6812 LED Test Starting...");
}

void loop() {
  // Sweep one at a time starting from D6
  Serial.println("--- Individual sweep ---");
  for (int i = 0; i < NUM_STRANDS; i++) {
    Serial.print("LED on D"); Serial.println(LED_PINS[i]);
    strands[i].fill(WHITE);
    strands[i].show();
    delay(500);
    strands[i].fill(OFF);
    strands[i].show();
    delay(200);
  }

  // All RED
  Serial.println("--- All RED ---");
  setAll(RED); delay(800);

  // All GREEN
  Serial.println("--- All GREEN ---");
  setAll(GREEN); delay(800);

  // All BLUE
  Serial.println("--- All BLUE ---");
  setAll(BLUE); delay(800);

  // All off
  Serial.println("--- All OFF ---");
  setAll(OFF); delay(1000);
}
