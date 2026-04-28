// NeutralLightTest — RP2040 (Feather)
// D6  = neutral indicator SK6812 (green when D9 pulled low)
// D9  = input with internal pull-up; wire to GND to trigger
// Startup: cycles all 4 SK6812 LEDs (D6, D7, D10, D11) so you can verify wiring

#include <Adafruit_NeoPixel.h>

static constexpr uint8_t PIN_TRIGGER = 9;

// One NeoPixel object per pin
static constexpr uint8_t LED_PINS[] = { 6, 7, 10, 11 };
static constexpr uint8_t NUM_LEDS   = sizeof(LED_PINS);

Adafruit_NeoPixel leds[NUM_LEDS] = {
  Adafruit_NeoPixel(1, LED_PINS[0], NEO_GRB + NEO_KHZ800),
  Adafruit_NeoPixel(1, LED_PINS[1], NEO_GRB + NEO_KHZ800),
  Adafruit_NeoPixel(1, LED_PINS[2], NEO_GRB + NEO_KHZ800),
  Adafruit_NeoPixel(1, LED_PINS[3], NEO_GRB + NEO_KHZ800),
};

// Match colors from main firmware: oil=red, eng=red, fuel=amber, neutral=green
static const uint32_t STARTUP_COLORS[] = {
  Adafruit_NeoPixel::Color(255,   0,   0),   // D6  oil warn  — red
  Adafruit_NeoPixel::Color(255,   0,   0),   // D7  eng warn  — red
  Adafruit_NeoPixel::Color(255, 100,   0),   // D10 fuel warn — amber
  Adafruit_NeoPixel::Color(  0, 255,   0),   // D11 neutral   — green
};

void allOff() {
  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    leds[i].setPixelColor(0, 0);
    leds[i].show();
  }
}

void startupCycle() {
  // Light each LED in sequence
  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    leds[i].setPixelColor(0, STARTUP_COLORS[i]);
    leds[i].show();
    delay(300);
    leds[i].setPixelColor(0, 0);
    leds[i].show();
    delay(100);
  }
  // Flash all on together once
  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    leds[i].setPixelColor(0, STARTUP_COLORS[i]);
    leds[i].show();
  }
  delay(400);
  allOff();
  delay(200);
}

void setup() {
  pinMode(PIN_TRIGGER, INPUT_PULLUP);
  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    leds[i].begin();
    leds[i].setBrightness(80);
  }
  allOff();
  startupCycle();
}

void loop() {
  // D6 = neutral light, green when D9 pulled low
  leds[0].setPixelColor(0, digitalRead(PIN_TRIGGER) == LOW
    ? Adafruit_NeoPixel::Color(0, 255, 0)
    : 0);
  leds[0].show();
  delay(20);
}
