// TransmitterCANTest.ino
//
// Simulates Bosch MS4.3 / PE3 ECU telemetry on an Adafruit Feather RP2040 RFM95
// and transmits three binary packet types over LoRa, matching the wire format
// used by Car/CarCanLoRaTx/CarCanLoRaTx.ino exactly.
//
// Packet types (all little-endian, preamble 0xA5):
//
//   0x01 — CAN frame (24 bytes):
//     [0xA5][0x01][flags][dlc][id:4LE][ts:4LE][data:8][seq:4LE]
//     flags: bit0=ext, bit1=rtr
//
//   0x02 — IMU (22 bytes):
//     [0xA5][0x02][imuSeq:4LE][ts:4LE]
//     [ax_mg:2][ay_mg:2][az_mg:2]   (int16, units = 0.001 g)
//     [gx_ddps:2][gy_ddps:2][gz_ddps:2] (int16, units = 0.1 dps)
//
//   0x03 — Analog sensors (14 bytes):
//     [0xA5][0x03][seq:4LE][ts:4LE][steer_ddeg:2][oil2_ddegC:2]
//     (int16, units = 0.1° / 0.1°C)
//
// CAN frame IDs simulated: 0x770, 0x771, 0x772, 0x790 (Bosch MS4.3 layout).
// Use with Testing/ReciverCANTest to verify end-to-end decode.

#include <Arduino.h>
#include <SPI.h>
#include <RH_RF95.h>
#include <math.h>

// =====================================================================
//  HARDWARE
// =====================================================================
#define RFM95_CS   16
#define RFM95_RST  17
#define RFM95_INT  21
#define RF95_FREQ  915.0f
#define CAN_CS     12   // pulled high — not used in test, just deselected

// =====================================================================
//  TIMING
// =====================================================================
static constexpr uint32_t CAN_PERIOD_MS    = 10;   // one CAN frame/cycle per slot
static constexpr uint32_t IMU_PERIOD_MS    = 10;   // 100 Hz IMU
static constexpr uint32_t ANALOG_PERIOD_MS = 20;   // 50 Hz analog
static constexpr uint32_t BLINK_MS         = 500;

// =====================================================================
//  CONSTANTS (match CarCanLoRaTx)
// =====================================================================
static constexpr float BAR_TO_PSI = 14.503774f;
static constexpr float PSI_TO_KPA = 6.894757f;

// =====================================================================
//  GLOBALS
// =====================================================================
RH_RF95 rf95(RFM95_CS, RFM95_INT);

// Simulated sensor values
float sim_rpm       = 0, sim_kph      = 0, sim_tps_pct  = 0;
float sim_ign_deg   = 0, sim_map_kpa  = 0, sim_lambda    = 0;
float sim_fuel_psi  = 0, sim_oil_psi  = 0, sim_batt_v    = 0;
float sim_clt_c     = 0, sim_iat_c    = 0, sim_oil_t1_c  = 0;
float sim_inj_ms    = 0, sim_steer_deg= 0, sim_oil_t2_c  = 0;
float sim_ax        = 0, sim_ay       = 0, sim_az        = 0;
float sim_gx        = 0, sim_gy       = 0, sim_gz        = 0;

// Sequence counters
uint32_t g_canSeq    = 0;
uint32_t g_imuSeq    = 0;
uint32_t g_analogSeq = 0;

uint32_t g_lastCan    = 0;
uint32_t g_lastImu    = 0;
uint32_t g_lastAnalog = 0;
uint32_t g_lastBlink  = 0;
bool     g_ledState   = false;

// CAN frame cycle state
static uint8_t s_770row  = 0;           // cycles 0-9
static uint8_t s_771idx  = 0;           // indexes into {0,1,3}
static uint8_t s_772idx  = 0;           // indexes into {0,3}
static uint8_t s_frameSlot = 0;         // 0-12: 10×0x770, 1×0x771, 1×0x772, 1×0x790

static const uint8_t k771rows[3] = {0, 1, 3};
static const uint8_t k772rows[2] = {0, 3};

// =====================================================================
//  WAVEFORM GENERATOR — realistic fake sensor values
// =====================================================================
void simulateSensors(uint32_t nowMs) {
  float t = nowMs / 1000.0f;

  sim_rpm      = 2500.0f + 1500.0f * (sinf(t * 0.9f)  * 0.5f + 0.5f);
  sim_kph      = sim_rpm / 85.0f;
  sim_tps_pct  = 20.0f  + 60.0f  * (sinf(t * 1.1f + 0.3f) * 0.5f + 0.5f);
  sim_ign_deg  = 12.0f  + 6.0f   * sinf(t * 0.5f);
  sim_map_kpa  = 90.0f  + 25.0f  * (sinf(t * 0.4f) * 0.5f + 0.5f);
  sim_lambda   = 0.97f  + 0.04f  * sinf(t * 1.7f);
  sim_fuel_psi = 55.0f  + 5.0f   * sinf(t * 0.3f);
  sim_oil_psi  = 50.0f  + 8.0f   * sinf(t * 0.3f + 1.2f);
  sim_batt_v   = 13.4f  + 0.3f   * sinf(t * 0.15f);
  sim_clt_c    = 82.0f  + 8.0f   * (sinf(t * 0.2f)  * 0.5f + 0.5f);
  sim_iat_c    = 35.0f  + 5.0f   * sinf(t * 0.8f);
  sim_oil_t1_c = 90.0f  + 10.0f  * (sinf(t * 0.15f) * 0.5f + 0.5f);
  sim_inj_ms   = 2.5f   + 0.8f   * sinf(t * 0.7f);
  sim_steer_deg= 25.0f  * sinf(t * 0.6f);
  sim_oil_t2_c = 88.0f  + 12.0f  * (sinf(t * 0.18f) * 0.5f + 0.5f);

  sim_ax = 0.05f * sinf(t * 2.3f);
  sim_ay = 0.10f * sinf(t * 1.8f + 0.5f);
  sim_az = 1.00f + 0.02f * sinf(t * 3.1f);
  sim_gx = 3.0f  * sinf(t * 2.0f);
  sim_gy = 2.0f  * sinf(t * 1.5f + 1.0f);
  sim_gz = 1.5f  * sinf(t * 1.2f + 0.3f);
}

// =====================================================================
//  PACKET HELPERS
// =====================================================================
static inline void putU16LE(uint8_t *p, uint16_t v) {
  p[0] = v & 0xFF; p[1] = v >> 8;
}
static inline void putI16LE(uint8_t *p, int16_t v) {
  putU16LE(p, (uint16_t)v);
}
static inline void putU32LE(uint8_t *p, uint32_t v) {
  p[0]=(uint8_t)(v);      p[1]=(uint8_t)(v>>8);
  p[2]=(uint8_t)(v>>16);  p[3]=(uint8_t)(v>>24);
}

// =====================================================================
//  BOSCH FRAME ENCODERS
//  Encoding reverses the decode formulas in CarCanLoRaTx updateTxSnapshot()
//  and printDecoded().
// =====================================================================

// 0x770 — RPM / VEH / TPS / IGN / row-muxed byte
// Decode refs: rpm = u16_lohi(d[1],d[2])*0.25   → raw = rpm/0.25 = rpm*4
//              kph = u16_lohi(d[3],d[4])*0.00781 → raw = kph/0.00781
//              tps = d[5]*0.391                  → raw = tps/0.391
//              ign = (int8_t)d[6]*1.365           → raw = ign/1.365
void make0x770(uint8_t *d, uint8_t row) {
  uint16_t raw_rpm = (uint16_t)(sim_rpm * 4.0f);
  uint16_t raw_kph = (uint16_t)(sim_kph / 0.00781f);
  uint8_t  raw_tps = (uint8_t) constrain(sim_tps_pct / 0.391f,  0, 255);
  int8_t   raw_ign = (int8_t)  constrain(sim_ign_deg / 1.365f, -128, 127);
  d[0] = row;
  d[1] = raw_rpm & 0xFF;  d[2] = raw_rpm >> 8;
  d[3] = raw_kph & 0xFF;  d[4] = raw_kph >> 8;
  d[5] = raw_tps;
  d[6] = (uint8_t)raw_ign;
  switch (row) {
    case 3: d[7] = (uint8_t)constrain(sim_clt_c    + 40.0f, 0, 255); break;
    case 4: d[7] = (uint8_t)constrain(sim_oil_t1_c + 39.0f, 0, 255); break;
    case 6: d[7] = (uint8_t)constrain(sim_iat_c    + 40.0f, 0, 255); break;
    case 9: d[7] = 1;  break;  // gear 1 placeholder
    default: d[7] = 0; break;
  }
}

// 0x771 — MAP / battery / fuel+oil pressures
// row 0: map_kpa = u16_lohi(d[5],d[6])*0.01        → raw = map_kpa*100
// row 1: batt_v  = d[7]*0.111                       → raw = batt_v/0.111
// row 3: fuel_psi = d[5]*0.0515*BAR_TO_PSI          → raw = fuel_psi/(0.0515*BAR_TO_PSI)
//        oil_psi  = d[7]*0.0513*BAR_TO_PSI          → raw = oil_psi/(0.0513*BAR_TO_PSI)
void make0x771(uint8_t *d, uint8_t row) {
  memset(d, 0, 8);
  d[0] = row;
  switch (row) {
    case 0: {
      uint16_t raw_map = (uint16_t)(sim_map_kpa * 100.0f);
      d[5] = raw_map & 0xFF;  d[6] = raw_map >> 8;
      break;
    }
    case 1:
      d[7] = (uint8_t)constrain(sim_batt_v / 0.111f, 0, 255);
      break;
    case 3:
      d[5] = (uint8_t)constrain(sim_fuel_psi / (0.0515f * BAR_TO_PSI), 0, 255);
      d[7] = (uint8_t)constrain(sim_oil_psi  / (0.0513f * BAR_TO_PSI), 0, 255);
      break;
  }
}

// 0x772 — lambda / injection time
// row 0: lambda  = u16_lohi(d[2],d[3])*0.000244 → raw = lambda/0.000244
// row 3: inj_ms  = d[2]*0.8192                  → raw = inj_ms/0.8192
void make0x772(uint8_t *d, uint8_t row) {
  memset(d, 0, 8);
  d[0] = row;
  switch (row) {
    case 0: {
      uint16_t raw_l = (uint16_t)(sim_lambda / 0.000244f);
      d[2] = raw_l & 0xFF;  d[3] = raw_l >> 8;  // L1
      d[4] = raw_l & 0xFF;  d[5] = raw_l >> 8;  // L2 (same)
      break;
    }
    case 3:
      d[2] = (uint8_t)constrain(sim_inj_ms / 0.8192f, 0, 255);
      break;
  }
}

// 0x790 — high-res RPM / ignition / MAP
// rpm    = u16_lohi(d[0],d[1])          → raw = (uint16_t)rpm
// ign    = u16_lohi(d[4],d[5])*0.1      → raw = ign_deg*10
// map    = u16_lohi(d[6],d[7])*0.01*PSI_TO_KPA → raw = (map_kpa/PSI_TO_KPA)*100
void make0x790(uint8_t *d) {
  memset(d, 0, 8);
  uint16_t raw_rpm = (uint16_t)sim_rpm;
  d[0] = raw_rpm & 0xFF;  d[1] = raw_rpm >> 8;
  uint16_t raw_ign = (uint16_t)(sim_ign_deg * 10.0f);
  d[4] = raw_ign & 0xFF;  d[5] = raw_ign >> 8;
  uint16_t raw_map = (uint16_t)((sim_map_kpa / PSI_TO_KPA) * 100.0f);
  d[6] = raw_map & 0xFF;  d[7] = raw_map >> 8;
}

// =====================================================================
//  LORA SENDERS  (match CarCanLoRaTx wire format exactly)
// =====================================================================
void sendCanFrame(uint32_t canId, const uint8_t *data, uint8_t dlc, uint32_t nowMs) {
  uint8_t pkt[24] = {0};
  pkt[0] = 0xA5;  pkt[1] = 0x01;
  pkt[2] = 0x00;  // flags: standard frame, no RTR
  pkt[3] = dlc;
  putU32LE(&pkt[4],  canId);
  putU32LE(&pkt[8],  nowMs);
  for (uint8_t i = 0; i < 8 && i < dlc; i++) pkt[12 + i] = data[i];
  g_canSeq++;
  putU32LE(&pkt[20], g_canSeq);
  rf95.send(pkt, sizeof(pkt));
  rf95.waitPacketSent();
}

void sendImu(uint32_t nowMs) {
  uint8_t pkt[22] = {0};
  pkt[0] = 0xA5;  pkt[1] = 0x02;
  g_imuSeq++;
  putU32LE(&pkt[2],  g_imuSeq);
  putU32LE(&pkt[6],  nowMs);
  putI16LE(&pkt[10], (int16_t)(sim_ax * 1000.0f));
  putI16LE(&pkt[12], (int16_t)(sim_ay * 1000.0f));
  putI16LE(&pkt[14], (int16_t)(sim_az * 1000.0f));
  putI16LE(&pkt[16], (int16_t)(sim_gx * 10.0f));
  putI16LE(&pkt[18], (int16_t)(sim_gy * 10.0f));
  putI16LE(&pkt[20], (int16_t)(sim_gz * 10.0f));
  rf95.send(pkt, sizeof(pkt));
  rf95.waitPacketSent();
}

void sendAnalog(uint32_t nowMs) {
  uint8_t pkt[14] = {0};
  pkt[0] = 0xA5;  pkt[1] = 0x03;
  g_analogSeq++;
  putU32LE(&pkt[2],  g_analogSeq);
  putU32LE(&pkt[6],  nowMs);
  putI16LE(&pkt[10], (int16_t)(sim_steer_deg * 10.0f));
  putI16LE(&pkt[12], (int16_t)(sim_oil_t2_c  * 10.0f));
  rf95.send(pkt, sizeof(pkt));
  rf95.waitPacketSent();
}

// =====================================================================
//  SETUP
// =====================================================================
void initRadio() {
  pinMode(CAN_CS, OUTPUT);
  digitalWrite(CAN_CS, HIGH);

  SPI.setRX(8);
  SPI.setTX(15);
  SPI.setSCK(14);
  SPI.begin();

  pinMode(RFM95_RST, OUTPUT);
  digitalWrite(RFM95_RST, HIGH); delay(10);
  digitalWrite(RFM95_RST, LOW);  delay(10);
  digitalWrite(RFM95_RST, HIGH); delay(10);

  if (!rf95.init()) {
    Serial.println(F("ERROR: LoRa init FAILED"));
    while (1) delay(100);
  }
  if (!rf95.setFrequency(RF95_FREQ)) {
    Serial.println(F("ERROR: setFrequency FAILED"));
    while (1) delay(100);
  }
  rf95.setTxPower(20, false);
  Serial.println(F("LoRa init OK @ 915 MHz  20 dBm"));
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);
  delay(500);
  Serial.println(F("\n--- Simulated Bosch MS4.3 -> LoRa TX ---"));
  initRadio();
}

// =====================================================================
//  LOOP
// =====================================================================
void loop() {
  uint32_t now = millis();

  if (now - g_lastBlink >= BLINK_MS) {
    g_lastBlink = now;
    g_ledState  = !g_ledState;
    digitalWrite(LED_BUILTIN, g_ledState);
  }

  simulateSensors(now);

  // ── CAN frames — one frame per period, cycling through all types ────
  // Slot layout (13 slots per full cycle):
  //   0-9  : 0x770 (one row per slot, rows 0-9)
  //   10   : 0x771 (cycling rows 0, 1, 3)
  //   11   : 0x772 (cycling rows 0, 3)
  //   12   : 0x790
  if (now - g_lastCan >= CAN_PERIOD_MS) {
    g_lastCan = now;
    uint8_t d[8] = {0};

    if (s_frameSlot <= 9) {
      make0x770(d, s_770row);
      sendCanFrame(0x770, d, 8, now);
      Serial.print(F("[TX 0x770] row=")); Serial.print(s_770row);
      Serial.print(F("  rpm="));  Serial.print(sim_rpm, 0);
      Serial.print(F("  kph="));  Serial.print(sim_kph, 1);
      Serial.print(F("  tps="));  Serial.print(sim_tps_pct, 1);
      Serial.println();
      s_770row = (s_770row + 1) % 10;

    } else if (s_frameSlot == 10) {
      uint8_t row = k771rows[s_771idx];
      make0x771(d, row);
      sendCanFrame(0x771, d, 8, now);
      Serial.print(F("[TX 0x771] row=")); Serial.println(row);
      s_771idx = (s_771idx + 1) % 3;

    } else if (s_frameSlot == 11) {
      uint8_t row = k772rows[s_772idx];
      make0x772(d, row);
      sendCanFrame(0x772, d, 8, now);
      Serial.print(F("[TX 0x772] row=")); Serial.println(row);
      s_772idx = (s_772idx + 1) % 2;

    } else {
      make0x790(d);
      sendCanFrame(0x790, d, 8, now);
      Serial.print(F("[TX 0x790] rpm=")); Serial.print(sim_rpm, 0);
      Serial.print(F("  ign="));          Serial.print(sim_ign_deg, 1);
      Serial.print(F("  map_kpa="));      Serial.println(sim_map_kpa, 1);
    }

    s_frameSlot = (s_frameSlot + 1) % 13;
  }

  // ── IMU ─────────────────────────────────────────────────────────────
  if (now - g_lastImu >= IMU_PERIOD_MS) {
    g_lastImu = now;
    sendImu(now);
  }

  // ── Analog sensors ───────────────────────────────────────────────────
  if (now - g_lastAnalog >= ANALOG_PERIOD_MS) {
    g_lastAnalog = now;
    sendAnalog(now);
    Serial.print(F("[TX 0x03] steer="));  Serial.print(sim_steer_deg, 1);
    Serial.print(F("  oil_t2="));         Serial.println(sim_oil_t2_c, 1);
  }
}
