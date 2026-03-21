#include <Arduino.h>
#include <SPI.h>
#include <RH_RF95.h>

// --------------------- USER SETTINGS ---------------------
static constexpr float LORA_FREQ_MHZ = 915.0;

// Force SPI pins (your known-good mapping)
static constexpr uint8_t PIN_MISO = 8;
static constexpr uint8_t PIN_SCK  = 14;
static constexpr uint8_t PIN_MOSI = 15;

// Keep CAN CS HIGH so it doesn't fight SPI (no CAN hardware on RX side)
static constexpr uint8_t CAN_CS = 5;

// RFM95 pins
static constexpr uint8_t RFM95_CS  = 16;
static constexpr uint8_t RFM95_INT = 21;
static constexpr uint8_t RFM95_RST = 17;

#ifndef PIN_LED
  #define PIN_LED LED_BUILTIN
#endif
static constexpr uint8_t LED_RED = PIN_LED;

// --------------------- Bosch MS4.3 (PE3 8400) IDs ---------------------
static constexpr uint32_t ID_PE3_770 = 0x770;
static constexpr uint32_t ID_PE3_771 = 0x771;
static constexpr uint32_t ID_PE3_772 = 0x772;
static constexpr uint32_t ID_PE3_790 = 0x790;

static constexpr float BAR_TO_PSI = 14.503774f;
static constexpr float PSI_TO_KPA = 6.894757f;

// --------------------- Bosch snapshot ---------------------
struct BoschSnapshot {
  bool seen = false;

  float rpm       = 0.0f;
  float veh_kph   = 0.0f;
  float tps_pct   = 0.0f;
  float ign_deg   = 0.0f;
  float coolant_c = NAN;
  float oil_temp_c = NAN;
  float air_c     = NAN;   // IAT — key name matches Dyno_Final.py
  uint8_t gear    = 0;

  float map_kpa     = 0.0f;
  float map_ext_kpa = 0.0f;
  float lambda1     = 0.0f;
  float lambda2     = 0.0f;

  float batt_v   = 0.0f;
  float fuel_psi = 0.0f;
  float oil_psi  = 0.0f;
  float inj_ms   = 0.0f;

  int16_t rssi = 0;
};

BoschSnapshot g_snap;
static uint32_t g_pkt = 0;

// --------------------- Helpers ---------------------
static inline uint32_t get_u32_le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint16_t u16_lohi(uint8_t lo, uint8_t hi) {
  return ((uint16_t)hi << 8) | lo;
}

static inline int16_t s16_lohi(uint8_t lo, uint8_t hi) {
  uint16_t n = u16_lohi(lo, hi);
  return (n > 32767) ? (int16_t)(n - 65536) : (int16_t)n;
}

static inline int16_t get_s16_le(const uint8_t *p) {
  return s16_lohi(p[0], p[1]);
}

// --------------------- Bosch decode ---------------------
void updateSnapshot(uint32_t id, bool ext, uint8_t dlc, const uint8_t *d) {
  if (ext) return; // Bosch uses standard 11-bit IDs only

  if (id == ID_PE3_770 && dlc >= 7) {
    g_snap.seen   = true;
    uint8_t row   = d[0];
    g_snap.rpm     = u16_lohi(d[1], d[2]) * 0.25f;
    g_snap.veh_kph = u16_lohi(d[3], d[4]) * 0.00781f;
    g_snap.tps_pct = d[5] * 0.391f;
    g_snap.ign_deg = (int8_t)d[6] * 1.365f;
    if (dlc >= 8) {
      if (row == 3) g_snap.coolant_c  = d[7] - 40.0f;
      else if (row == 4) g_snap.oil_temp_c = d[7] - 39.0f;
      else if (row == 6) g_snap.air_c   = d[7] - 40.0f;
      else if (row == 9) g_snap.gear    = d[7];
    }
  }
  else if (id == ID_PE3_771 && dlc >= 6) {
    g_snap.seen = true;
    uint8_t row = d[0];
    if (row == 0 && dlc >= 7) {
      g_snap.map_kpa = u16_lohi(d[5], d[6]) * 0.01f;
    } else if (row == 1 && dlc >= 8) {
      g_snap.batt_v = d[7] * 0.111f;
    } else if (row == 3 && dlc >= 8) {
      g_snap.fuel_psi = d[5] * 0.0515f * BAR_TO_PSI;
      g_snap.oil_psi  = d[7] * 0.0513f * BAR_TO_PSI;
    }
  }
  else if (id == ID_PE3_772 && dlc >= 3) {
    g_snap.seen = true;
    uint8_t row = d[0];
    if (row == 0 && dlc >= 6) {
      g_snap.lambda1 = u16_lohi(d[2], d[3]) * 0.000244f;
      g_snap.lambda2 = u16_lohi(d[4], d[5]) * 0.000244f;
    } else if (row == 3) {
      g_snap.inj_ms = d[2] * 0.8192f;
    }
  }
  else if (id == ID_PE3_790) {
    g_snap.seen = true;
    if (dlc >= 2) g_snap.rpm = u16_lohi(d[0], d[1]);
    if (dlc >= 6) g_snap.ign_deg = u16_lohi(d[4], d[5]) * 0.1f;
    if (dlc >= 8) g_snap.map_ext_kpa = u16_lohi(d[6], d[7]) * 0.01f * PSI_TO_KPA;
  }
}

// --------------------- JSON emit ---------------------
void emitJson() {
  // Dyno_Final.py keys: rpm, tps_pct, map_kpa, batt_v, coolant_c, air_c
  // Extra fields are ignored by the default key list but show in raw table
  Serial.print(F("{\"pkt\":"));    Serial.print(g_pkt);
  Serial.print(F(",\"rpm\":"));    Serial.print(g_snap.rpm, 1);
  Serial.print(F(",\"tps_pct\":")); Serial.print(g_snap.tps_pct, 2);
  Serial.print(F(",\"map_kpa\":")); Serial.print(g_snap.map_kpa, 2);
  Serial.print(F(",\"batt_v\":")); Serial.print(g_snap.batt_v, 2);

  Serial.print(F(",\"coolant_c\":"));
  if (isnan(g_snap.coolant_c)) Serial.print(F("null"));
  else Serial.print(g_snap.coolant_c, 1);

  Serial.print(F(",\"air_c\":"));
  if (isnan(g_snap.air_c)) Serial.print(F("null"));
  else Serial.print(g_snap.air_c, 1);

  Serial.print(F(",\"ign_deg\":"));  Serial.print(g_snap.ign_deg, 2);
  Serial.print(F(",\"veh_kph\":"));  Serial.print(g_snap.veh_kph, 2);
  Serial.print(F(",\"lambda1\":")); Serial.print(g_snap.lambda1, 3);
  Serial.print(F(",\"lambda2\":")); Serial.print(g_snap.lambda2, 3);
  Serial.print(F(",\"fuel_psi\":")); Serial.print(g_snap.fuel_psi, 1);
  Serial.print(F(",\"oil_psi\":"));  Serial.print(g_snap.oil_psi, 1);
  Serial.print(F(",\"inj_ms\":"));   Serial.print(g_snap.inj_ms, 2);
  Serial.print(F(",\"gear\":"));     Serial.print(g_snap.gear);
  Serial.print(F(",\"rssi\":"));     Serial.print(g_snap.rssi);
  Serial.println(F("}"));
}

// --------------------- LoRa ---------------------
RH_RF95 rf95(RFM95_CS, RFM95_INT);

uint32_t lastBlink  = 0;
uint32_t lastEmit   = 0;
uint32_t lastStatus = 0;
uint32_t pktCount   = 0;
bool ledState = false;

static constexpr uint32_t EMIT_INTERVAL_MS   = 100;   // 10 Hz JSON output
static constexpr uint32_t STATUS_INTERVAL_MS = 5000;  // heartbeat to serial

void setup() {
  pinMode(LED_RED, OUTPUT);
  digitalWrite(LED_RED, LOW);

  Serial.begin(115200);
  delay(600);

  SPI.setRX(PIN_MISO);
  SPI.setTX(PIN_MOSI);
  SPI.setSCK(PIN_SCK);
  SPI.begin();

  pinMode(CAN_CS, OUTPUT);
  digitalWrite(CAN_CS, HIGH);

  pinMode(RFM95_RST, OUTPUT);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);
  digitalWrite(RFM95_RST, LOW);
  delay(10);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);

  Serial.println(F("# CAN->LoRa RX | Bosch JSON output"));

  if (!rf95.init()) {
    Serial.println(F("# ERROR: rf95.init() failed"));
    while (1) { digitalWrite(LED_RED, !digitalRead(LED_RED)); delay(100); }
  }
  if (!rf95.setFrequency(LORA_FREQ_MHZ)) {
    Serial.println(F("# ERROR: setFrequency failed"));
    while (1) { digitalWrite(LED_RED, !digitalRead(LED_RED)); delay(100); }
  }
  rf95.setTxPower(20, false);

  Serial.print(F("# LoRa OK @ ")); Serial.print(LORA_FREQ_MHZ); Serial.println(F(" MHz"));
  Serial.println(F("# Waiting for Bosch CAN frames via LoRa..."));
}

void loop() {
  uint32_t now = millis();
  if (now - lastBlink >= 500) {
    lastBlink = now;
    ledState = !ledState;
    digitalWrite(LED_RED, ledState);
  }

  // Drain all available LoRa packets before emitting
  while (rf95.available()) {
    uint8_t buf[RH_RF95_MAX_MESSAGE_LEN];
    uint8_t len = sizeof(buf);
    if (!rf95.recv(buf, &len)) break;

    if (len < 10 || buf[0] != 0xA5) continue; // bad magic

    if (buf[1] == 0x01 && len == 24) {
      // CAN frame packet
      uint8_t  flags = buf[2];
      bool     ext   = (flags & 0x01) != 0;
      uint8_t  dlc   = buf[3] & 0x0F;
      uint32_t id    = get_u32_le(&buf[4]);

      g_pkt = get_u32_le(&buf[20]);
      g_snap.rssi = rf95.lastRssi();
      pktCount++;

      updateSnapshot(id, ext, dlc, &buf[12]);

    } else if (buf[1] == 0x02 && len == 22) {
      // IMU packet — emit JSON immediately (separate line from Bosch)
      uint32_t seq   = get_u32_le(&buf[2]);
      float ax = get_s16_le(&buf[10]) / 1000.0f;
      float ay = get_s16_le(&buf[12]) / 1000.0f;
      float az = get_s16_le(&buf[14]) / 1000.0f;
      float gx = get_s16_le(&buf[16]) / 10.0f;
      float gy = get_s16_le(&buf[18]) / 10.0f;
      float gz = get_s16_le(&buf[20]) / 10.0f;
      pktCount++;

      Serial.print(F("{\"t\":\"imu\",\"pkt\":"));  Serial.print(seq);
      Serial.print(F(",\"ax_g\":"));    Serial.print(ax, 3);
      Serial.print(F(",\"ay_g\":"));    Serial.print(ay, 3);
      Serial.print(F(",\"az_g\":"));    Serial.print(az, 3);
      Serial.print(F(",\"gx_dps\":")); Serial.print(gx, 2);
      Serial.print(F(",\"gy_dps\":")); Serial.print(gy, 2);
      Serial.print(F(",\"gz_dps\":")); Serial.print(gz, 2);
      Serial.print(F(",\"rssi\":"));   Serial.print(rf95.lastRssi());
      Serial.println(F("}"));
    }
  }

  // Emit JSON at fixed rate (10 Hz)
  if (g_snap.seen && (now - lastEmit >= EMIT_INTERVAL_MS)) {
    lastEmit = now;
    emitJson();
  }

  // Periodic heartbeat (ignored by Dyno_Final.py — starts with #)
  if (now - lastStatus >= STATUS_INTERVAL_MS) {
    lastStatus = now;
    Serial.print(F("# [RX] uptime="));  Serial.print(now / 1000);
    Serial.print(F("s  pkts="));        Serial.print(pktCount);
    Serial.print(F("  rssi="));         Serial.print(g_snap.rssi);
    Serial.print(F("  seen="));         Serial.println(g_snap.seen ? F("YES") : F("NO - waiting..."));
  }
}
