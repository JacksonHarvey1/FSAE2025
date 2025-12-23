#include <Arduino.h>

static uint32_t lastSend = 0;
static uint32_t counter = 0;

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  Serial.begin(115200);
  delay(800);

  Serial.println("# RP2040 <-> Pi serial link up");
  Serial.println("ts_ms,counter,rpm,map_kpa");  // CSV header
}

void handleCommand(const String &cmd) {
  if (cmd == "PING") {
    Serial.println("PONG");
  } else if (cmd == "LED ON") {
    digitalWrite(LED_BUILTIN, HIGH);
    Serial.println("OK LED ON");
  } else if (cmd == "LED OFF") {
    digitalWrite(LED_BUILTIN, LOW);
    Serial.println("OK LED OFF");
  } else if (cmd.startsWith("RATE ")) {
    Serial.print("OK ");
    Serial.println(cmd); // you can parse and set output rate later
  } else {
    Serial.print("ERR unknown cmd: ");
    Serial.println(cmd);
  }
}

void loop() {
  // ---- Read commands from Pi ----
  while (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() > 0) handleCommand(cmd);
  }

  // ---- Periodic telemetry output ----
  uint32_t now = millis();
  if (now - lastSend >= 100) {  // 10 Hz
    lastSend = now;

    // Fake values for now (replace with real CAN values later)
    uint32_t rpm = 1000 + (counter % 3000);
    float map_kpa = 95.0f + (counter % 20);

    Serial.print(now); Serial.print(",");
    Serial.print(counter++); Serial.print(",");
    Serial.print(rpm); Serial.print(",");
    Serial.println(map_kpa, 2);
  }

  // slow blink (proof running)
  static uint32_t lastBlink = 0;
  static bool led = false;
  if (now - lastBlink > 500) {
    lastBlink = now;
    led = !led;
    digitalWrite(LED_BUILTIN, led);
  }
}
