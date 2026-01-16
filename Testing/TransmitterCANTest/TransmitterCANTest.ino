// TransmitterCANTest.ino
//
// Simulates PE3 (AN400) CAN frames on an Adafruit Feather RP2040 CAN board
// and publishes the condensed telemetry snapshot over LoRa (RFM95/RH_RF95).
// The packet layout mirrors the scaling used in Dyno/DynoRP204CANHATINtegration
// so the receiver can sanity-check values without needing the full JSON output.
//
// Packet format (all little-endian):
//   Byte 0  : 0xAA preamble
//   Byte 1  : protocol version (currently 0x01)
//   Byte 2  : sequence number (wraps at 255)
//   Byte 3  : payload length in bytes (should be 34)
//   Byte 4-7  : uint32 timestamp (ms since boot)
//   Byte 8-9  : uint16 rpm (1 rpm/bit)
//   Byte 10-11: int16  tps_pct_tenths (0.1 %/bit)
//   Byte 12-13: int16  fot_ms_tenths (0.1 ms/bit)
//   Byte 14-15: int16  ign_deg_tenths (0.1 deg/bit)
//   Byte 16-17: uint16 map_kpa_centi (0.01 kPa/bit)
//   Byte 18-19: uint16 baro_kpa_centi (0.01 kPa/bit)
//   Byte 20-21: uint16 lambda_milli (0.001/bit)
//   Byte 22-23: int16  oil_psi_tenths (0.1 psi/bit)
//   Byte 24-25: uint16 batt_v_centi (0.01 V/bit)
//   Byte 26-27: int16  coolant_c_tenths (0.1 C/bit)
//   Byte 28-29: int16  air_c_tenths (0.1 C/bit)
//   Byte 30-37: uint16 wheel speed Hz_tenths for FL, FR, BL, BR (0.1 Hz/bit)
//
// LoRa settings match Testing/TransmitterTest.ino so this sketch can be paired
// with Testing/ReciverCANTest/ReciverCANTest.ino for a full end-to-end check.

#include <Arduino.h>
#include <SPI.h>
#include <RH_RF95.h>
#include <math.h>

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
static constexpr uint32_t SEND_PERIOD_MS     = 200;   // 5 Hz snapshot
static constexpr uint32_t BLINK_MS           = 500;

RH_RF95 rf95(RFM95_CS, RFM95_INT);

struct TelemetrySnapshot {
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

TelemetrySnapshot g_snapshot = {};
uint8_t           g_seq      = 0;
uint32_t          g_lastSend = 0;
uint32_t          g_lastBlink = 0;
bool              g_ledState = false;

static inline uint16_t clampU16(float v) {
  if (v < 0.0f) return 0;
  if (v > 65535.0f) return 65535;
  return static_cast<uint16_t>(v + 0.5f);
}

static inline int16_t clampS16(float v) {
  if (v < -32768.0f) return -32768;
  if (v > 32767.0f)  return 32767;
  return static_cast<int16_t>(v + (v >= 0 ? 0.5f : -0.5f));
}

static inline void writeU16LE(uint8_t *buf, size_t &idx, uint16_t value) {
  buf[idx++] = value & 0xFF;
  buf[idx++] = value >> 8;
}

static inline void writeS16LE(uint8_t *buf, size_t &idx, int16_t value) {
  writeU16LE(buf, idx, static_cast<uint16_t>(value));
}

static inline void writeU32LE(uint8_t *buf, size_t &idx, uint32_t value) {
  buf[idx++] = (uint8_t)(value & 0xFF);
  buf[idx++] = (uint8_t)((value >> 8) & 0xFF);
  buf[idx++] = (uint8_t)((value >> 16) & 0xFF);
  buf[idx++] = (uint8_t)((value >> 24) & 0xFF);
}

void simulatePe3Snapshot(uint32_t nowMs) {
  // Basic waveform generators to mimic live CAN data.
  float t = nowMs / 1000.0f;

  g_snapshot.rpm      = 2500 + (uint16_t)(1200.0f * (sinf(t * 0.9f) * 0.5f + 0.5f));
  g_snapshot.tps_pct  = 30.0f + 50.0f * (sinf(t * 1.3f + 0.5f) * 0.5f + 0.5f);
  g_snapshot.fot_ms   = 2.2f + 0.8f * (sinf(t * 0.7f + 1.0f) * 0.5f + 0.5f);
  g_snapshot.ign_deg  = 12.0f + 6.0f * sinf(t * 0.5f);

  g_snapshot.map_kpa  = 95.0f + 20.0f * (sinf(t * 0.4f) * 0.5f + 0.5f);
  g_snapshot.baro_kpa = 101.3f;
  g_snapshot.lambda   = 0.96f + 0.04f * sinf(t * 1.7f);

  g_snapshot.oil_psi  = 50.0f + 8.0f * sinf(t * 0.3f + 1.2f);
  g_snapshot.batt_v   = 13.4f + 0.25f * sinf(t * 0.15f);
  g_snapshot.coolant_c = 82.0f + 6.0f * (sinf(t * 0.25f) * 0.5f + 0.5f);
  g_snapshot.air_c     = 32.0f + 4.0f * sinf(t * 0.9f);

  float wsBase = (g_snapshot.rpm / 60.0f) / 4.4f; // crude wheel speed estimate (Hz)
  g_snapshot.ws_fl_hz = max(0.0f, wsBase * (1.00f + 0.02f * sinf(t * 0.8f)));
  g_snapshot.ws_fr_hz = max(0.0f, wsBase * (0.98f + 0.03f * sinf(t * 0.6f + 0.5f)));
  g_snapshot.ws_bl_hz = max(0.0f, wsBase * (1.01f + 0.02f * sinf(t * 0.75f + 1.0f)));
  g_snapshot.ws_br_hz = max(0.0f, wsBase * (0.99f + 0.03f * sinf(t * 0.55f + 1.3f)));
}

size_t buildTelemetryPacket(uint8_t *outBuf, size_t maxLen, uint32_t nowMs) {
  if (maxLen < PACKET_BYTES) return 0;

  size_t idx = 0;
  outBuf[idx++] = LORA_PREAMBLE;
  outBuf[idx++] = LORA_VERSION;
  outBuf[idx++] = g_seq++;
  outBuf[idx++] = LORA_PAYLOAD_BYTES;

  writeU32LE(outBuf, idx, nowMs);
  writeU16LE(outBuf, idx, g_snapshot.rpm);
  writeS16LE(outBuf, idx, clampS16(g_snapshot.tps_pct * 10.0f));
  writeS16LE(outBuf, idx, clampS16(g_snapshot.fot_ms * 10.0f));
  writeS16LE(outBuf, idx, clampS16(g_snapshot.ign_deg * 10.0f));
  writeU16LE(outBuf, idx, clampU16(g_snapshot.map_kpa * 100.0f));
  writeU16LE(outBuf, idx, clampU16(g_snapshot.baro_kpa * 100.0f));
  writeU16LE(outBuf, idx, clampU16(g_snapshot.lambda * 1000.0f));
  writeS16LE(outBuf, idx, clampS16(g_snapshot.oil_psi * 10.0f));
  writeU16LE(outBuf, idx, clampU16(g_snapshot.batt_v * 100.0f));
  writeS16LE(outBuf, idx, clampS16(g_snapshot.coolant_c * 10.0f));
  writeS16LE(outBuf, idx, clampS16(g_snapshot.air_c * 10.0f));
  writeU16LE(outBuf, idx, clampU16(g_snapshot.ws_fl_hz * 10.0f));
  writeU16LE(outBuf, idx, clampU16(g_snapshot.ws_fr_hz * 10.0f));
  writeU16LE(outBuf, idx, clampU16(g_snapshot.ws_bl_hz * 10.0f));
  writeU16LE(outBuf, idx, clampU16(g_snapshot.ws_br_hz * 10.0f));

  return idx;
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

  rf95.setTxPower(13, false);
  Serial.println("LoRa init OK @ 915 MHz");
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  Serial.begin(115200);
  Serial.println("\n--- Simulated PE3 CAN → LoRa TX ---");

  initRadio();
}

void loop() {
  uint32_t now = millis();

  if (now - g_lastBlink >= BLINK_MS) {
    g_lastBlink = now;
    g_ledState = !g_ledState;
    digitalWrite(LED_BUILTIN, g_ledState);
  }

  simulatePe3Snapshot(now);

  if (now - g_lastSend >= SEND_PERIOD_MS) {
    g_lastSend = now;

    uint8_t packet[PACKET_BYTES] = {0};
    size_t bytes = buildTelemetryPacket(packet, sizeof(packet), now);
    if (bytes > 0) {
      rf95.send(packet, bytes);
      rf95.waitPacketSent();

      Serial.print("[TX] seq=");
      Serial.print(packet[2]);
      Serial.print(" rpm=");
      Serial.print(g_snapshot.rpm);
      Serial.print(" tps=%");
      Serial.print(g_snapshot.tps_pct, 1);
      Serial.print(" map_kpa=");
      Serial.print(g_snapshot.map_kpa, 1);
      Serial.print(" lambda=");
      Serial.print(g_snapshot.lambda, 3);
      Serial.print(" oil_psi=");
      Serial.print(g_snapshot.oil_psi, 1);
      Serial.println();
    }
  }
}
