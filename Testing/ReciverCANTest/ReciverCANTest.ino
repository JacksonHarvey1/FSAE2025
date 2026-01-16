// ReciverCANTest.ino
//
// Companion sketch for Testing/TransmitterCANTest.ino.
// Listens for LoRa packets containing simulated PE3 telemetry snapshots, parses
// the binary payload, and prints the decoded values to Serial for inspection.
//
// See TransmitterCANTest for the full packet layout. Key fields replicated here:
//   Byte 0  : 0xAA preamble
//   Byte 1  : protocol version (expect 0x01)
//   Byte 2  : sequence number
//   Byte 3  : payload length (should be 34)
//   Byte 4-7: timestamp (ms)
//   Remaining bytes: packed telemetry channels (rpm, TPS, MAP, lambda, etc.)
//
// Usage:
//   1. Flash TransmitterCANTest onto one RP2040 Feather CAN board with RFM95.
//   2. Flash this sketch on another board (or run sequentially).
//   3. Open two Serial monitors at 115200 baud. You should see matching
//      snapshots flowing every 200 ms along with RSSI information.

#include <Arduino.h>
#include <SPI.h>
#include <RH_RF95.h>

#define RFM95_CS   16
#define RFM95_RST  17
#define RFM95_INT  21
#define RF95_FREQ  915.0

// If a CAN wing / other SPI device is stacked:
#define CAN_CS     5

static constexpr uint8_t  LORA_PREAMBLE      = 0xAA;
static constexpr uint8_t  LORA_VERSION       = 0x01;
static constexpr uint8_t  LORA_PAYLOAD_BYTES = 34;
static constexpr uint8_t  PACKET_BYTES       = 4 + LORA_PAYLOAD_BYTES;
static constexpr uint32_t BLINK_MS           = 400;

RH_RF95 rf95(RFM95_CS, RFM95_INT);

struct TelemetrySnapshot {
  uint32_t ts_ms;
  uint16_t rpm;
  float    tps_pct;
  float    fot_ms;
  float    ign_deg;
  float    map_kpa;
  float    baro_kpa;
  float    lambda;
  float    oil_psi;
  float    batt_v;
  float    coolant_c;
  float    air_c;
  float    ws_fl_hz;
  float    ws_fr_hz;
  float    ws_bl_hz;
  float    ws_br_hz;
};

TelemetrySnapshot g_lastSnapshot = {};
uint8_t           g_lastSeq      = 0;
bool              g_havePacket   = false;
uint32_t          g_lastBlink    = 0;
bool              g_ledState     = false;

static inline uint16_t readU16LE(const uint8_t *buf, size_t &idx) {
  uint16_t value = (uint16_t)buf[idx] | ((uint16_t)buf[idx + 1] << 8);
  idx += 2;
  return value;
}

static inline int16_t readS16LE(const uint8_t *buf, size_t &idx) {
  return (int16_t)readU16LE(buf, idx);
}

static inline uint32_t readU32LE(const uint8_t *buf, size_t &idx) {
  uint32_t value = (uint32_t)buf[idx] |
                   ((uint32_t)buf[idx + 1] << 8) |
                   ((uint32_t)buf[idx + 2] << 16) |
                   ((uint32_t)buf[idx + 3] << 24);
  idx += 4;
  return value;
}

bool decodePacket(const uint8_t *buf, uint8_t len, TelemetrySnapshot &out) {
  if (len != PACKET_BYTES) {
    Serial.print("[RX] Unexpected length: ");
    Serial.println(len);
    return false;
  }

  if (buf[0] != LORA_PREAMBLE || buf[1] != LORA_VERSION || buf[3] != LORA_PAYLOAD_BYTES) {
    Serial.println("[RX] Header mismatch, dropping packet");
    return false;
  }

  size_t idx = 4;
  out.ts_ms     = readU32LE(buf, idx);
  out.rpm       = readU16LE(buf, idx);
  out.tps_pct   = readS16LE(buf, idx) / 10.0f;
  out.fot_ms    = readS16LE(buf, idx) / 10.0f;
  out.ign_deg   = readS16LE(buf, idx) / 10.0f;
  out.map_kpa   = readU16LE(buf, idx) / 100.0f;
  out.baro_kpa  = readU16LE(buf, idx) / 100.0f;
  out.lambda    = readU16LE(buf, idx) / 1000.0f;
  out.oil_psi   = readS16LE(buf, idx) / 10.0f;
  out.batt_v    = readU16LE(buf, idx) / 100.0f;
  out.coolant_c = readS16LE(buf, idx) / 10.0f;
  out.air_c     = readS16LE(buf, idx) / 10.0f;
  out.ws_fl_hz  = readU16LE(buf, idx) / 10.0f;
  out.ws_fr_hz  = readU16LE(buf, idx) / 10.0f;
  out.ws_bl_hz  = readU16LE(buf, idx) / 10.0f;
  out.ws_br_hz  = readU16LE(buf, idx) / 10.0f;

  return true;
}

void initRadio() {
  pinMode(CAN_CS, OUTPUT);
  digitalWrite(CAN_CS, HIGH);

  SPI.setRX(8);
  SPI.setTX(15);
  SPI.setSCK(14);
  SPI.begin();

  pinMode(RFM95_RST, OUTPUT);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);
  digitalWrite(RFM95_RST, LOW);
  delay(10);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);

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

void printSnapshot(const TelemetrySnapshot &snap, int16_t rssi, uint8_t seq) {
  Serial.print("[RX] seq=");
  Serial.print(seq);
  Serial.print(" ts=");
  Serial.print(snap.ts_ms);
  Serial.print(" rpm=");
  Serial.print(snap.rpm);
  Serial.print(" tps=%");
  Serial.print(snap.tps_pct, 1);
  Serial.print(" map_kpa=");
  Serial.print(snap.map_kpa, 1);
  Serial.print(" lambda=");
  Serial.print(snap.lambda, 3);
  Serial.print(" oil_psi=");
  Serial.print(snap.oil_psi, 1);
  Serial.print(" coolant_c=");
  Serial.print(snap.coolant_c, 1);
  Serial.print(" batt_v=");
  Serial.print(snap.batt_v, 2);
  Serial.print(" ws_fl=");
  Serial.print(snap.ws_fl_hz, 1);
  Serial.print(" ws_fr=");
  Serial.print(snap.ws_fr_hz, 1);
  Serial.print(" ws_bl=");
  Serial.print(snap.ws_bl_hz, 1);
  Serial.print(" ws_br=");
  Serial.print(snap.ws_br_hz, 1);
  Serial.print(" RSSI=");
  Serial.println(rf95.lastRssi());
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  Serial.begin(115200);
  Serial.println("\n--- LoRa RX for simulated PE3 telemetry ---");

  initRadio();
}

void loop() {
  uint32_t now = millis();

  if (now - g_lastBlink >= BLINK_MS) {
    g_lastBlink = now;
    g_ledState = !g_ledState;
    digitalWrite(LED_BUILTIN, g_ledState);
  }

  if (rf95.available()) {
    uint8_t buf[RH_RF95_MAX_MESSAGE_LEN];
    uint8_t len = sizeof(buf);

    if (rf95.recv(buf, &len)) {
      if (len >= PACKET_BYTES && buf[0] == LORA_PREAMBLE) {
        TelemetrySnapshot snap;
        if (decodePacket(buf, len, snap)) {
          g_lastSeq = buf[2];
          g_lastSnapshot = snap;
          g_havePacket = true;
          printSnapshot(g_lastSnapshot, rf95.lastRssi(), g_lastSeq);
        }
      } else {
        Serial.println("[RX] Received non-telemetry packet");
      }
    }
  }
}
