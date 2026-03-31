// LED Test - SK6812 addressable LEDs on D5, D6, D9, D10, D11, D12
// Requires: Adafruit NeoPixel library (install via Arduino Library Manager)
//
// Each pin drives one SK6812 LED (adjust LEDS_PER_PIN if you have chains)
// SK6812 uses NEO_GRBW for RGBW variant or NEO_GRB for RGB variant

#include <Adafruit_NeoPixel.h>

#define LEDS_PER_PIN 1  // Change if you have multiple LEDs chained per pin

const int LED_PINS[] = {5, 6, 9, 10, 11, 12};
const int NUM_STRANDS = 6;

Adafruit_NeoPixel strands[NUM_STRANDS] = {
  Adafruit_NeoPixel(LEDS_PER_PIN, 5,  NEO_GRB + NEO_KHZ800),
  Adafruit_NeoPixel(LEDS_PER_PIN, 6,  NEO_GRB + NEO_KHZ800),
  Adafruit_NeoPixel(LEDS_PER_PIN, 9,  NEO_GRB + NEO_KHZ800),
  Adafruit_NeoPixel(LEDS_PER_PIN, 10, NEO_GRB + NEO_KHZ800),
  Adafruit_NeoPixel(LEDS_PER_PIN, 11, NEO_GRB + NEO_KHZ800),
  Adafruit_NeoPixel(LEDS_PER_PIN, 12, NEO_GRB + NEO_KHZ800),
};

// Test colors
const uint32_t RED   = Adafruit_NeoPixel::Color(255, 0, 0);
const uint32_t GREEN = Adafruit_NeoPixel::Color(0, 255, 0);
const uint32_t BLUE  = Adafruit_NeoPixel::Color(0, 0, 255);
const uint32_t WHITE = Adafruit_NeoPixel::Color(255, 255, 255);
const uint32_t OFF   = Adafruit_NeoPixel::Color(0, 0, 0);

void setAll(uint32_t color) {
  for (int i = 0; i < NUM_STRANDS; i++) {
    strands[i].fill(color);
    strands[i].show();
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("SK6812 LED Test Starting...");

  for (int i = 0; i < NUM_STRANDS; i++) {
    strands[i].begin();
    strands[i].setBrightness(128);  // 50% brightness (0-255)
    strands[i].clear();
    strands[i].show();
  }
}

void loop() {
  // Sweep through each LED one at a time (white)
  Serial.println("--- Individual LED sweep (white) ---");
  for (int i = 0; i < NUM_STRANDS; i++) {
    Serial.print("LED on D");
    Serial.println(LED_PINS[i]);
    strands[i].fill(WHITE);
    strands[i].show();
    delay(500);
    strands[i].fill(OFF);
    strands[i].show();
    delay(200);
  }

  // Color test: all RED
  Serial.println("--- All RED ---");
  setAll(RED);
  delay(800);

  // Color test: all GREEN
  Serial.println("--- All GREEN ---");
  setAll(GREEN);
  delay(800);

  // Color test: all BLUE
  Serial.println("--- All BLUE ---");
  setAll(BLUE);
  delay(800);

  // All off
  Serial.println("--- All OFF ---");
  setAll(OFF);
  delay(1000);
}
