#include <Arduino.h>
#include <SPI.h>
#include <RH_RF95.h>

// --------------------- USER SETTINGS ---------------------
static constexpr float LORA_FREQ_MHZ = 915.0;

// Force SPI pins (your known-good mapping)
static constexpr uint8_t PIN_MISO = 8;
static constexpr uint8_t PIN_SCK  = 14;
static constexpr uint8_t PIN_MOSI = 15;

// If you have a CAN wing stacked on RX too, keep its CS HIGH so it doesn't fight SPI
static constexpr uint8_t CAN_CS = 5;

// RFM95 pins (matches your working test)
static constexpr uint8_t RFM95_CS  = 16;
static constexpr uint8_t RFM95_INT = 21;
static constexpr uint8_t RFM95_RST = 17;

// LED
#ifndef PIN_LED
  #define PIN_LED LED_BUILTIN
#endif
static constexpr uint8_t LED_RED = PIN_LED;

RH_RF95 rf95(RFM95_CS, RFM95_INT);

static inline uint32_t get_u32_le(const uint8_t *p) {
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

uint32_t lastBlink = 0;
bool ledState = false;

void setup() {
  pinMode(LED_RED, OUTPUT);
  digitalWrite(LED_RED, LOW);

  Serial.begin(115200);
  delay(600);

  // Force SPI pins
  SPI.setRX(PIN_MISO);
  SPI.setTX(PIN_MOSI);
  SPI.setSCK(PIN_SCK);
  SPI.begin();

  // Keep CAN deselected (prevents your earlier init failure)
  pinMode(CAN_CS, OUTPUT);
  digitalWrite(CAN_CS, HIGH);

  // Reset radio
  pinMode(RFM95_RST, OUTPUT);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);
  digitalWrite(RFM95_RST, LOW);
  delay(10);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);

  Serial.println("=== CAN->LoRa RX (binary) ===");

  if (!rf95.init()) {
    Serial.println("ERROR: rf95.init() failed");
    while (1) { digitalWrite(LED_RED, !digitalRead(LED_RED)); delay(100); }
  }
  if (!rf95.setFrequency(LORA_FREQ_MHZ)) {
    Serial.println("ERROR: setFrequency failed");
    while (1) { digitalWrite(LED_RED, !digitalRead(LED_RED)); delay(100); }
  }
  rf95.setTxPower(20, false);

  Serial.print("LoRa OK @ "); Serial.print(LORA_FREQ_MHZ); Serial.println(" MHz");
  Serial.println("Waiting for 24-byte CAN packets...");
}

void loop() {
  // Red LED blink every 0.5 seconds (RX requirement)
  uint32_t now = millis();
  if (now - lastBlink >= 500) {
    lastBlink = now;
    ledState = !ledState;
    digitalWrite(LED_RED, ledState);
  }

  if (!rf95.available()) return;

  uint8_t buf[RH_RF95_MAX_MESSAGE_LEN];
  uint8_t len = sizeof(buf);

  if (!rf95.recv(buf, &len)) return;

  int16_t rssi = rf95.lastRssi();

  if (len != 24 || buf[0] != 0xA5 || buf[1] != 0x01) {
    Serial.print("[RX] bad packet len="); Serial.print(len);
    Serial.print(" rssi="); Serial.println(rssi);
    return;
  }

  uint8_t flags = buf[2];
  bool ext = (flags & 0x01) != 0;
  bool rtr = (flags & 0x02) != 0;
  uint8_t dlc = buf[3] & 0x0F;

  uint32_t id    = get_u32_le(&buf[4]);
  uint32_t ts_ms = get_u32_le(&buf[8]);
  uint32_t seq   = get_u32_le(&buf[20]);

  Serial.print("[RX] seq="); Serial.print(seq);
  Serial.print(" ts_ms="); Serial.print(ts_ms);
  Serial.print(" ID=0x"); Serial.print(id, HEX);
  Serial.print(ext ? " EXT" : " STD");
  if (rtr) Serial.print(" RTR");
  Serial.print(" DLC="); Serial.print(dlc);
  Serial.print(" DATA=");

  for (uint8_t i = 0; i < dlc && i < 8; i++) {
    if (buf[12 + i] < 16) Serial.print('0');
    Serial.print(buf[12 + i], HEX);
    if (i + 1 < dlc) Serial.print(' ');
  }

  Serial.print(" | RSSI="); Serial.println(rssi);
}
