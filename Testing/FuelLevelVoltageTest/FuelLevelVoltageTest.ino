// FuelLevelVoltageTest
// Reads the fuel level sensor (PANW 103395-395 NTC thermistor) on A1 (GP27)
// and prints the raw ADC count and calculated voltage to Serial.
// Wiring: 3.3V -> 10kΩ pull-up -> A1 -> thermistor -> GND
// Board: Adafruit Feather RP2040

static constexpr uint8_t  PIN_FUEL = A1;   // GP27
static constexpr uint32_t PRINT_INTERVAL_MS = 200;  // 5 Hz

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000);
  analogReadResolution(12);
  pinMode(PIN_FUEL, INPUT);
  Serial.println("FuelLevelVoltageTest running — A1 (GP27)");
  Serial.println("raw_adc, voltage_V");
}

void loop() {
  static uint32_t lastPrint = 0;
  uint32_t now = millis();
  if (now - lastPrint >= PRINT_INTERVAL_MS) {
    lastPrint = now;
    int raw = analogRead(PIN_FUEL);
    float voltage = raw * (3.3f / 4095.0f);
    Serial.print(raw);
    Serial.print(", ");
    Serial.println(voltage, 4);
  }
}
