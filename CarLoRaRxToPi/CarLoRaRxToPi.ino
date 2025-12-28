// CarLoRaRxToPi.ino
//
// Feather RP2040 RFM95 (LoRa) acting as pit-side receiver.
//
// Role: Receive compact binary telemetry packets from CarCanLoRaTx over
// LoRa, decode them into engineering units, and emit one NDJSON line per
// packet over USB serial to a Raspberry Pi.
//
// Expected binary payload (little endian, 22 bytes):
//   Byte 0-1  : uint16  pkt      (packet counter, wraps at 65535)
//   Byte 2-3  : uint16  rpm      (1 rpm / bit)
//   Byte 4-5  : uint16  map_raw  (map_kpa * 10, 0.1 kPa / bit)
//   Byte 6-7  : uint16  tps_raw  (tps_pct * 10, 0.1 % / bit)
//   Byte 8-9  : int16   oil_raw  (oil_psi * 10, 0.1 psi / bit)
//   Byte 10-11: int16   clt_raw  (coolant_c * 10, 0.1 °C / bit)
//   Byte 12   : uint8   lam_raw  (lambda * 100, 0.01 / bit)
//   Byte 13   : uint8   batt_raw (batt_v * 10, 0.1 V / bit)
//   Byte 14-15: uint16  ws_fl    (ws_fl_hz * 2, 0.5 Hz / bit)
//   Byte 16-17: uint16  ws_fr    (ws_fr_hz * 2)
//   Byte 18-19: uint16  ws_bl    (ws_bl_hz * 2)
//   Byte 20-21: uint16  ws_br    (ws_br_hz * 2)
//
// Output NDJSON per packet, example:
//   {"ts_ms":123456,"pkt":42,"src":"car-lora","node_id":1,
//    "rpm":5234,"map_kpa":98.3,"tps_pct":12.3,
//    "oil_psi":52.1,"coolant_c":84.2,
//    "lambda":0.98,"batt_v":12.7,
//    "ws_fl_hz":0.0,"ws_fr_hz":0.0,"ws_bl_hz":0.0,"ws_br_hz":0.0}
//
// Lines starting with '#' are debug and ignored by the Pi ingestor.

#include <Arduino.h>
#include <SPI.h>
#include <RH_RF95.h>

// -------------------- LoRa radio config (built-in RFM95) --------------------

#define RFM95_CS   16
#define RFM95_RST  17
#define RFM95_INT  21

#define RF95_FREQ  915.0

RH_RF95 rf95(RFM95_CS, RFM95_INT);

// If a CAN wing / other SPI device is stacked, keep it deselected.
#define CAN_CS     5

// LED blink timing
const uint32_t BLINK_MS = 500;
uint32_t lastBlink = 0;
bool ledState = false;

// -------------------- Helpers to unpack little-endian fields ---------------

uint16_t read_u16_le(const uint8_t *buf, int idx) {
  return (uint16_t)buf[idx] | ((uint16_t)buf[idx + 1] << 8);
}

int16_t read_i16_le(const uint8_t *buf, int idx) {
  return (int16_t)read_u16_le(buf, idx);
}

// -------------------- NDJSON output ----------------------------------------

void printTelemetryJson(const uint8_t *buf, uint8_t len) {
  if (len < 22) return;  // invalid packet

  uint16_t pkt      = read_u16_le(buf, 0);
  uint16_t rpm      = read_u16_le(buf, 2);
  uint16_t map_raw  = read_u16_le(buf, 4);
  uint16_t tps_raw  = read_u16_le(buf, 6);
  int16_t  oil_raw  = read_i16_le(buf, 8);
  int16_t  clt_raw  = read_i16_le(buf, 10);
  uint8_t  lam_raw  = buf[12];
  uint8_t  batt_raw = buf[13];
  uint16_t ws_fl    = read_u16_le(buf, 14);
  uint16_t ws_fr    = read_u16_le(buf, 16);
  uint16_t ws_bl    = read_u16_le(buf, 18);
  uint16_t ws_br    = read_u16_le(buf, 20);

  // Convert to engineering units (mirror CarCanLoRaTx scaling)
  float map_kpa   = map_raw  / 10.0f;
  float tps_pct   = tps_raw  / 10.0f;
  float oil_psi   = oil_raw  / 10.0f;
  float coolant_c = clt_raw  / 10.0f;
  float lambda    = lam_raw  / 100.0f;
  float batt_v    = batt_raw / 10.0f;

  float ws_fl_hz  = ws_fl / 2.0f;
  float ws_fr_hz  = ws_fr / 2.0f;
  float ws_bl_hz  = ws_bl / 2.0f;
  float ws_br_hz  = ws_br / 2.0f;

  uint32_t ts_ms = millis();

  // Emit NDJSON line. Use the same style as CAN_to_Pi_USB.ino so the
  // Pi ingestor can treat this identically.

  Serial.print('{');

  auto add_kv = [](const char *key) {
    Serial.print('"');
    Serial.print(key);
    Serial.print('"');
    Serial.print(':');
  };

  // ts_ms, pkt, src, node_id
  add_kv("ts_ms");   Serial.print(ts_ms); Serial.print(',');
  add_kv("pkt");     Serial.print(pkt);   Serial.print(',');
  add_kv("src");     Serial.print("\"car-lora\""); Serial.print(',');
  add_kv("node_id"); Serial.print(1);     Serial.print(',');

  // core channels
  add_kv("rpm");       Serial.print(rpm);        Serial.print(',');
  add_kv("map_kpa");   Serial.print(map_kpa, 1); Serial.print(',');
  add_kv("tps_pct");   Serial.print(tps_pct, 1); Serial.print(',');
  add_kv("oil_psi");   Serial.print(oil_psi, 1); Serial.print(',');
  add_kv("coolant_c"); Serial.print(coolant_c, 1); Serial.print(',');
  add_kv("lambda");    Serial.print(lambda, 3);  Serial.print(',');
  add_kv("batt_v");    Serial.print(batt_v, 2);  Serial.print(',');

  // wheel speeds
  add_kv("ws_fl_hz"); Serial.print(ws_fl_hz, 1); Serial.print(',');
  add_kv("ws_fr_hz"); Serial.print(ws_fr_hz, 1); Serial.print(',');
  add_kv("ws_bl_hz"); Serial.print(ws_bl_hz, 1); Serial.print(',');
  add_kv("ws_br_hz"); Serial.print(ws_br_hz, 1);

  Serial.println('}');
}

// -------------------- Setup / loop -----------------------------------------

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  Serial.begin(115200);
  while (!Serial) {
    delay(10);
  }

  Serial.println("# CarLoRaRxToPi starting");

  // Make sure any stacked CAN wing is deselected
  pinMode(CAN_CS, OUTPUT);
  digitalWrite(CAN_CS, HIGH);

  // Use default SPI pins for the RFM95 on Feather RP2040
  SPI.setRX(8);
  SPI.setTX(15);
  SPI.setSCK(14);
  SPI.begin();

  // Reset radio
  pinMode(RFM95_RST, OUTPUT);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);
  digitalWrite(RFM95_RST, LOW);
  delay(10);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);

  if (!rf95.init()) {
    Serial.println("# ERROR: LoRa radio init FAILED");
    while (1) {
      digitalWrite(LED_BUILTIN, HIGH);
      delay(200);
      digitalWrite(LED_BUILTIN, LOW);
      delay(800);
    }
  }

  if (!rf95.setFrequency(RF95_FREQ)) {
    Serial.println("# ERROR: setFrequency FAILED");
    while (1) {
      digitalWrite(LED_BUILTIN, HIGH);
      delay(100);
      digitalWrite(LED_BUILTIN, LOW);
      delay(100);
    }
  }

  // Optional: match TX power / modem settings to TX node later if needed
  rf95.setTxPower(13, false);

  Serial.print("# LoRa RX ready @ ");
  Serial.print(RF95_FREQ);
  Serial.println(" MHz");
}

void loop() {
  uint32_t now = millis();

  // Blink LED to show liveness
  if (now - lastBlink >= BLINK_MS) {
    lastBlink = now;
    ledState = !ledState;
    digitalWrite(LED_BUILTIN, ledState);
  }

  // Receive packets and forward as NDJSON
  if (rf95.available()) {
    uint8_t buf[RH_RF95_MAX_MESSAGE_LEN];
    uint8_t len = sizeof(buf);

    if (rf95.recv(buf, &len)) {
      // Debug: raw length + RSSI
      Serial.print("# RX len=");
      Serial.print(len);
      Serial.print(" RSSI=");
      Serial.println(rf95.lastRssi());

      printTelemetryJson(buf, len);
    }
  }
}

