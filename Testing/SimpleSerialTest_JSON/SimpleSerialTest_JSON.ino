// SimpleSerialTest_JSON.ino
// Test sending JSON data similar to the Dyno telemetry system
// This will help us determine if the issue is with JSON parsing or data volume

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  
  delay(1000);
  
  Serial.println("# SimpleSerialTest_JSON starting...");
  Serial.println("# Sending JSON packets every 50ms (~20 Hz)");
  Serial.println("# Baud: 115200");
  Serial.println("#");
}

void loop() {
  static unsigned long pkt = 0;
  static unsigned long lastSend = 0;
  static bool ledState = false;
  
  unsigned long now = millis();
  
  // Send JSON every 50ms (~20 Hz, same as Dyno)
  if (now - lastSend >= 50) {
    lastSend = now;
    pkt++;
    
    // Build a JSON packet similar to the Dyno telemetry
    // This is a simplified version with fewer fields
    Serial.print("{");
    Serial.print("\"ts_ms\":");
    Serial.print(now);
    Serial.print(",\"pkt\":");
    Serial.print(pkt);
    Serial.print(",\"src\":\"can\"");
    Serial.print(",\"node_id\":1");
    Serial.print(",\"rpm\":");
    Serial.print(random(2000, 4000));
    Serial.print(",\"tps_pct\":");
    Serial.print(random(0, 100) * 0.1, 1);
    Serial.print(",\"map_kpa\":");
    Serial.print(random(80, 120) * 0.1, 2);
    Serial.print(",\"batt_v\":");
    Serial.print(110 + random(20), 2);
    Serial.print(",\"coolant_c\":");
    Serial.print(80 + random(20) * 0.1, 1);
    Serial.print(",\"air_c\":");
    Serial.print(20 + random(10) * 0.1, 1);
    Serial.println("}");
    
    // Blink LED every 500ms
    if (pkt % 10 == 0) {
      ledState = !ledState;
      digitalWrite(LED_BUILTIN, ledState);
    }
  }
}
