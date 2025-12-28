#include <Arduino.h>
#include <SPI.h>
#include <RH_RF95.h>

#define RFM95_CS   16
#define RFM95_RST  17
#define RFM95_INT  21
#define RF95_FREQ  915.0

// If a CAN wing / other SPI device is stacked:
#define CAN_CS     5

RH_RF95 rf95(RFM95_CS, RFM95_INT);

// LED blink timing (RX: 0.5s)
const uint32_t BLINK_MS = 500;
uint32_t lastBlink = 0;
bool ledState = false;

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  Serial.begin(115200);
  Serial.println("\n--- LoRa RX starting ---");

  pinMode(CAN_CS, OUTPUT);
  digitalWrite(CAN_CS, HIGH);

  // Force SPI pins to Feather defaults (MISO=8, MOSI=15, SCK=14)
  SPI.setRX(8);
  SPI.setTX(15);
  SPI.setSCK(14);
  SPI.begin();

  // Reset radio
  pinMode(RFM95_RST, OUTPUT);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);
  digitalWrite(RFM95_RST, LOW);  delay(10);
  digitalWrite(RFM95_RST, HIGH); delay(10);

  if (!rf95.init()) {
    Serial.println("ERROR: LoRa radio init FAILED");
    while (1) delay(100);
  }

  if (!rf95.setFrequency(RF95_FREQ)) {
    Serial.println("ERROR: setFrequency FAILED");
    while (1) delay(100);
  }

  Serial.println("LoRa init OK, listening...");
}

void loop() {
  uint32_t now = millis();

  // Blink LED every 0.5 seconds
  if (now - lastBlink >= BLINK_MS) {
    lastBlink = now;
    ledState = !ledState;
    digitalWrite(LED_BUILTIN, ledState);
  }

  // Receive and print packets
  if (rf95.available()) {
    uint8_t buf[RH_RF95_MAX_MESSAGE_LEN];
    uint8_t len = sizeof(buf);

    if (rf95.recv(buf, &len)) {
      Serial.print("[RX] ");
      Serial.write(buf, len);
      Serial.print(" | RSSI=");
      Serial.println(rf95.lastRssi());
    }
  }
}
