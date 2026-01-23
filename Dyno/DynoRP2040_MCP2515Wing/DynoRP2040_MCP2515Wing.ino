// DynoRP2040_MCP2515Wing.ino
//
// Adafruit Feather RP2040 + FeatherWing MCP2515 (SPI) stack
//   → Sniffs PE3 AN400 CAN frames at 250 kbps (extended IDs)
//   → Maintains a decoded telemetry snapshot in engineering units
//   → Emits NDJSON telemetry (~20 Hz) over USB for the Raspberry Pi ingest stack.
//
// This sketch is derived from Archive/2026/CAN_to_Pi_USB/CAN_to_Pi_USB.ino but
// is trimmed and documented specifically for the Feather RP2040 + MCP2515 wing
// combo we now use on the dyno. Pay attention to the pin table below to avoid
// the wiring mistakes we have run into previously.
//
// NDJSON line format (one JSON object per line):
//   { "ts_ms":..., "pkt":..., "src":"can", "node_id":1, "rpm":..., ... }
// Lines beginning with '#' are status/debug and should be ignored by the Pi.

#include <Arduino.h>
#include <SPI.h>

// ===== Feather RP2040 ↔ MCP2515 FeatherWing wiring =====
// • SPI runs on the secondary RP2040 SPI bus (spi1):
//     MISO = GP8   (Feather D22)  → Wing DO
//     MOSI = GP15  (Feather D23)  → Wing DI
//     SCK  = GP14  (Feather D24)  → Wing SCK
// • Chip-select is GP5 (Feather D5). Keep every other SPI device CS HIGH.
// • The FeatherWing does not expose STANDBY/RESET by default, so we leave them
//   pulled by on-board resistors. If you run wires for them later, set the
//   optional pins below.
// • INT is optional. We default to polling CANINTF so an unwired INT pin is ok.

//#ifndef SPI1_MISO
//  #error "This sketch requires RP2040 hardware with spi1 (Feather RP2040)."
//#endif

static constexpr uint8_t PIN_CAN_MISO   = 8;   // GP8  - SPI1 MISO
static constexpr uint8_t PIN_CAN_MOSI   = 15;  // GP15 - SPI1 MOSI
static constexpr uint8_t PIN_CAN_SCK    = 14;  // GP14 - SPI1 SCK
static constexpr uint8_t PIN_CAN_CS     = 5;   // GP5  - dedicated CS for MCP2515
static constexpr int8_t  PIN_CAN_INT    = -1;  // set to actual GPIO if you wire INT
static constexpr int8_t  PIN_CAN_STANDBY = -1; // optional standby pin
static constexpr int8_t  PIN_CAN_RESET   = -1; // optional reset pin

// Toggle to 1 if INT is wired and you want level-triggered reception.
#define USE_CAN_INTERRUPT 0

SPIClassRP2040 CAN_SPI(spi1, PIN_CAN_MISO, PIN_CAN_CS, PIN_CAN_SCK, PIN_CAN_MOSI);

// ===== MCP2515 register helpers =====
#define CMD_RESET   0xC0
#define CMD_READ    0x03
#define CMD_WRITE   0x02
#define CMD_BITMOD  0x05

#define CANSTAT  0x0E
#define CANCTRL  0x0F
#define CNF3     0x28
#define CNF2     0x29
#define CNF1     0x2A
#define CANINTE  0x2B
#define CANINTF  0x2C
#define EFLG     0x2D
#define TEC      0x1C
#define REC      0x1D
#define RXB0CTRL 0x60
#define RXB1CTRL 0x70
#define RXB0SIDH 0x61
#define RXB1SIDH 0x71

#define RX0IF 0x01
#define RX1IF 0x02
#define MODE_MASK   0xE0
#define MODE_CONFIG 0x80
#define MODE_NORMAL 0x00

struct Frame {
  uint32_t id;
  bool     ext;
  uint8_t  dlc;
  uint8_t  d[8];
};

static inline void csLow()  { digitalWrite(PIN_CAN_CS, LOW); }
static inline void csHigh() { digitalWrite(PIN_CAN_CS, HIGH); }

uint8_t mcpRead(uint8_t addr) {
  csLow();
  CAN_SPI.transfer(CMD_READ);
  CAN_SPI.transfer(addr);
  uint8_t v = CAN_SPI.transfer(0x00);
  csHigh();
  return v;
}

void mcpWrite(uint8_t addr, uint8_t val) {
  csLow();
  CAN_SPI.transfer(CMD_WRITE);
  CAN_SPI.transfer(addr);
  CAN_SPI.transfer(val);
  csHigh();
}

void mcpBitMod(uint8_t addr, uint8_t mask, uint8_t data) {
  csLow();
  CAN_SPI.transfer(CMD_BITMOD);
  CAN_SPI.transfer(addr);
  CAN_SPI.transfer(mask);
  CAN_SPI.transfer(data);
  csHigh();
}

void mcpReset() {
  csLow();
  CAN_SPI.transfer(CMD_RESET);
  csHigh();
  delay(10);
}

bool mcpSetMode(uint8_t mode) {
  mcpBitMod(CANCTRL, MODE_MASK, mode);
  delay(5);
  return ((mcpRead(CANSTAT) & MODE_MASK) == mode);
}

// Bit timing for AN400: 250 kbps @ 16 MHz crystal on the FeatherWing
bool mcpInit250k_16MHz() {
  mcpReset();
  if (!mcpSetMode(MODE_CONFIG)) return false;

  mcpWrite(CNF1, 0x41);
  mcpWrite(CNF2, 0xF1);
  mcpWrite(CNF3, 0x85);

  mcpWrite(RXB0CTRL, 0x60); // receive-any
  mcpWrite(RXB1CTRL, 0x60);
  mcpWrite(CANINTE, RX0IF | RX1IF);
  mcpWrite(CANINTF, 0x00);

  return mcpSetMode(MODE_NORMAL);
}

bool readRx(uint8_t base, Frame &f) {
  uint8_t sidh = mcpRead(base + 0);
  uint8_t sidl = mcpRead(base + 1);
  uint8_t eid8 = mcpRead(base + 2);
  uint8_t eid0 = mcpRead(base + 3);
  uint8_t dlc  = mcpRead(base + 4) & 0x0F;

  bool ext = (sidl & 0x08) != 0;
  uint32_t id = 0;

  if (ext) {
    uint32_t sid = ((uint32_t)sidh << 3) | (sidl >> 5);
    uint32_t eid = ((uint32_t)(sidl & 0x03) << 16) | ((uint32_t)eid8 << 8) | eid0;
    id = (sid << 18) | eid;
  } else {
    id = ((uint32_t)sidh << 3) | (sidl >> 5);
  }

  f.id  = id;
  f.ext = ext;
  f.dlc = dlc;

  memset(f.d, 0, sizeof(f.d));
  for (uint8_t i = 0; i < dlc && i < 8; i++) {
    f.d[i] = mcpRead(base + 5 + i);
  }
  return true;
}

void printFrameDebug(const Frame &f) {
  Serial.print("DBG RX ");
  Serial.print(f.ext ? "EXT" : "STD");
  Serial.print(" ID=0x");
  Serial.print(f.id, HEX);
  Serial.print(" DLC=");
  Serial.print(f.dlc);
  Serial.print(" DATA=");
  for (uint8_t i = 0; i < f.dlc; i++) {
    if (f.d[i] < 16) Serial.print('0');
    Serial.print(f.d[i], HEX);
    if (i + 1 < f.dlc) Serial.print(' ');
  }
  Serial.println();
}

void hardFail(const __FlashStringHelper *msg) {
  Serial.println(msg);
  while (true) {
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    delay(120);
  }
}

void logPinout() {
  Serial.println("# Wiring summary (RP2040 → MCP2515 wing):");
  Serial.print("#   CS=GP");   Serial.print(PIN_CAN_CS);
  Serial.print("  SCK=GP");   Serial.print(PIN_CAN_SCK);
  Serial.print("  MOSI=GP");  Serial.print(PIN_CAN_MOSI);
  Serial.print("  MISO=GP");  Serial.println(PIN_CAN_MISO);
  Serial.print("#   INT=");    Serial.println(PIN_CAN_INT >= 0 ? String(PIN_CAN_INT) : String("(unused)"));
  Serial.print("#   STBY=");   Serial.println(PIN_CAN_STANDBY >= 0 ? String(PIN_CAN_STANDBY) : String("(pull-down on wing)"));
  Serial.print("#   RESET=");  Serial.println(PIN_CAN_RESET >= 0 ? String(PIN_CAN_RESET) : String("(pull-up on wing)"));
}

// ===== Telemetry snapshot =====
static uint32_t g_pkt_counter     = 0;
static bool     g_seen_any_frame  = false;

static uint16_t g_rpm             = 0;
static float    g_tps_pct         = 0.0f;
static float    g_fot_ms          = 0.0f;
static float    g_ign_deg         = 0.0f;
static float    g_rpm_rate        = 0.0f;
static float    g_tps_rate        = 0.0f;
static float    g_map_rate        = 0.0f;
static float    g_maf_load_rate   = 0.0f;

static float    g_baro_kpa        = 0.0f;
static float    g_map_kpa         = 0.0f;
static float    g_lambda          = 0.0f;
static float    g_lambda1         = 0.0f;
static float    g_lambda2         = 0.0f;
static float    g_lambda_target   = 0.0f;

static float    g_oil_psi         = 0.0f;
static float    g_batt_v          = 0.0f;
static float    g_coolant_c       = 0.0f;
static float    g_air_c           = 0.0f;

static float    g_ws_fl_hz        = 0.0f;
static float    g_ws_fr_hz        = 0.0f;
static float    g_ws_bl_hz        = 0.0f;
static float    g_ws_br_hz        = 0.0f;

static float    g_ai_v[8]         = {0};
static float    g_therm5_temp     = 0.0f;
static float    g_therm7_temp     = 0.0f;

static float    g_rpm_rate        = 0.0f;
static float    g_tps_rate        = 0.0f;
static float    g_map_rate        = 0.0f;
static float    g_maf_load_rate   = 0.0f;

static float    g_pwm_duty_pct[8] = {0};
static float    g_percent_slip    = 0.0f;
static float    g_driven_wheel_roc_ft_s2 = 0.0f;
static float    g_traction_desired_pct   = 0.0f;
static float    g_driven_avg_ws_ft_s     = 0.0f;
static float    g_nondriven_avg_ws_ft_s  = 0.0f;
static float    g_ign_comp_deg          = 0.0f;
static float    g_ign_cut_pct           = 0.0f;
static float    g_driven_ws1_ft_s       = 0.0f;
static float    g_driven_ws2_ft_s       = 0.0f;
static float    g_nondriven_ws1_ft_s    = 0.0f;
static float    g_nondriven_ws2_ft_s    = 0.0f;
static float    g_fuel_comp_accel_pct   = 0.0f;
static float    g_fuel_comp_start_pct   = 0.0f;
static float    g_fuel_comp_air_pct     = 0.0f;
static float    g_fuel_comp_coolant_pct = 0.0f;
static float    g_fuel_comp_baro_pct    = 0.0f;
static float    g_fuel_comp_map_pct     = 0.0f;
static float    g_ign_comp_air_deg      = 0.0f;
static float    g_ign_comp_coolant_deg  = 0.0f;
static float    g_ign_comp_baro_deg     = 0.0f;
static float    g_ign_comp_map_deg      = 0.0f;

// Helpers
static inline uint16_t u16_lohi(uint8_t lo, uint8_t hi) {
  return (uint16_t)hi << 8 | lo;
}

static inline int16_t s16_lohi(uint8_t lo, uint8_t hi) {
  uint16_t n = u16_lohi(lo, hi);
  return (n > 32767) ? (int16_t)(n - 65536) : (int16_t)n;
}

static const uint32_t ID_PE1  = 0x0CFFF048;
static const uint32_t ID_PE2  = 0x0CFFF148;
static const uint32_t ID_PE3  = 0x0CFFF248;
static const uint32_t ID_PE4  = 0x0CFFF348;
static const uint32_t ID_PE5  = 0x0CFFF448;
static const uint32_t ID_PE6  = 0x0CFFF548;
static const uint32_t ID_PE7  = 0x0CFFF648;
static const uint32_t ID_PE8  = 0x0CFFF748;
static const uint32_t ID_PE9  = 0x0CFFF848;
static const uint32_t ID_PE10 = 0x0CFFF948;
static const uint32_t ID_PE11 = 0x0CFFFA48;
static const uint32_t ID_PE12 = 0x0CFFFB48;
static const uint32_t ID_PE13 = 0x0CFFFC48;
static const uint32_t ID_PE14 = 0x0CFFFD48;
static const uint32_t ID_PE15 = 0x0CFFFE48;
static const uint32_t ID_PE16 = 0x0CFFD048;

void updateTelemetryFromFrame(const Frame &f) {
  if (!f.ext) return;
  g_seen_any_frame = true;

  if (f.id == ID_PE1 && f.dlc >= 8) {
    g_rpm     = u16_lohi(f.d[0], f.d[1]);
    g_tps_pct = s16_lohi(f.d[2], f.d[3]) * 0.1f;
    g_fot_ms  = s16_lohi(f.d[4], f.d[5]) * 0.1f;
    g_ign_deg = s16_lohi(f.d[6], f.d[7]) * 0.1f;
  }
  else if (f.id == ID_PE2 && f.dlc >= 8) {
    float baro_raw = s16_lohi(f.d[0], f.d[1]) * 0.01f;
    float map_raw  = s16_lohi(f.d[2], f.d[3]) * 0.01f;
    float lam_raw  = s16_lohi(f.d[4], f.d[5]) * 0.01f;
    bool  kpa      = (f.d[6] & 0x01) != 0;

    if (kpa) {
      g_baro_kpa = baro_raw;
      g_map_kpa  = map_raw;
    } else {
      constexpr float PSI_TO_KPA = 6.89476f;
      g_baro_kpa = baro_raw * PSI_TO_KPA;
      g_map_kpa  = map_raw  * PSI_TO_KPA;
    }
    g_lambda1 = lam_raw;
    g_lambda  = lam_raw;
  }
  else if (f.id == ID_PE3 && f.dlc >= 8) {
    g_ai_v[0] = s16_lohi(f.d[0], f.d[1]) * 0.001f;
    g_ai_v[1] = s16_lohi(f.d[2], f.d[3]) * 0.001f;
    g_ai_v[2] = s16_lohi(f.d[4], f.d[5]) * 0.001f;
    g_ai_v[3] = s16_lohi(f.d[6], f.d[7]) * 0.001f;
  }
  else if (f.id == ID_PE4 && f.dlc >= 8) {
    g_ai_v[4] = s16_lohi(f.d[0], f.d[1]) * 0.001f;
    g_ai_v[5] = s16_lohi(f.d[2], f.d[3]) * 0.001f;
    g_ai_v[6] = s16_lohi(f.d[4], f.d[5]) * 0.001f;
    g_ai_v[7] = s16_lohi(f.d[6], f.d[7]) * 0.001f;
  }
  else if (f.id == ID_PE5 && f.dlc >= 8) {
    g_ws_fr_hz = s16_lohi(f.d[0], f.d[1]) * 0.2f;
    g_ws_fl_hz = s16_lohi(f.d[2], f.d[3]) * 0.2f;
    g_ws_br_hz = s16_lohi(f.d[4], f.d[5]) * 0.2f;
    g_ws_bl_hz = s16_lohi(f.d[6], f.d[7]) * 0.2f;
  }
  else if (f.id == ID_PE6 && f.dlc >= 8) {
    float batt_v  = s16_lohi(f.d[0], f.d[1]) * 0.01f;
    float air_raw = s16_lohi(f.d[2], f.d[3]) * 0.1f;
    float clt_raw = s16_lohi(f.d[4], f.d[5]) * 0.1f;
    bool  temp_c  = (f.d[6] & 0x01) != 0;

    g_batt_v = batt_v;
    if (temp_c) {
      g_air_c     = air_raw;
      g_coolant_c = clt_raw;
    } else {
      g_air_c     = (air_raw - 32.0f) * (5.0f / 9.0f);
      g_coolant_c = (clt_raw - 32.0f) * (5.0f / 9.0f);
    }
  }
  else if (f.id == ID_PE7 && f.dlc >= 4) {
    g_therm5_temp = s16_lohi(f.d[0], f.d[1]) * 0.1f;
    g_therm7_temp = s16_lohi(f.d[2], f.d[3]) * 0.1f;
  }
  else if (f.id == ID_PE8 && f.dlc >= 8) {
    g_rpm_rate      = s16_lohi(f.d[0], f.d[1]) * 1.0f;
    g_tps_rate      = s16_lohi(f.d[2], f.d[3]) * 1.0f;
    g_map_rate      = s16_lohi(f.d[4], f.d[5]) * 1.0f;
    g_maf_load_rate = s16_lohi(f.d[6], f.d[7]) * 0.1f;
  }
  else if (f.id == ID_PE9 && f.dlc >= 2) {
    g_lambda1 = s16_lohi(f.d[0], f.d[1]) * 0.01f;
    g_lambda  = g_lambda1;
    if (f.dlc >= 4) g_lambda2       = s16_lohi(f.d[2], f.d[3]) * 0.01f;
    if (f.dlc >= 6) g_lambda_target = s16_lohi(f.d[4], f.d[5]) * 0.01f;
  }
  else if (f.id == ID_PE10 && f.dlc >= 1) {
    for (uint8_t i = 0; i < min<uint8_t>(8, f.dlc); ++i) {
      g_pwm_duty_pct[i] = f.d[i] * 0.5f;
    }
  }
  else if (f.id == ID_PE11 && f.dlc >= 6) {
    g_percent_slip          = s16_lohi(f.d[0], f.d[1]) * 0.1f;
    g_driven_wheel_roc_ft_s2 = s16_lohi(f.d[2], f.d[3]) * 0.1f;
    g_traction_desired_pct   = s16_lohi(f.d[4], f.d[5]) * 0.1f;
  }
  else if (f.id == ID_PE12 && f.dlc >= 8) {
    g_driven_avg_ws_ft_s    = s16_lohi(f.d[0], f.d[1]) * 0.1f;
    g_nondriven_avg_ws_ft_s = s16_lohi(f.d[2], f.d[3]) * 0.1f;
    g_ign_comp_deg          = s16_lohi(f.d[4], f.d[5]) * 0.1f;
    g_ign_cut_pct           = s16_lohi(f.d[6], f.d[7]) * 0.1f;
  }
  else if (f.id == ID_PE13 && f.dlc >= 8) {
    g_driven_ws1_ft_s    = s16_lohi(f.d[0], f.d[1]) * 0.1f;
    g_driven_ws2_ft_s    = s16_lohi(f.d[2], f.d[3]) * 0.1f;
    g_nondriven_ws1_ft_s = s16_lohi(f.d[4], f.d[5]) * 0.1f;
    g_nondriven_ws2_ft_s = s16_lohi(f.d[6], f.d[7]) * 0.1f;
  }
  else if (f.id == ID_PE14 && f.dlc >= 8) {
    g_fuel_comp_accel_pct   = s16_lohi(f.d[0], f.d[1]) * 0.1f;
    g_fuel_comp_start_pct   = s16_lohi(f.d[2], f.d[3]) * 0.1f;
    g_fuel_comp_air_pct     = s16_lohi(f.d[4], f.d[5]) * 0.1f;
    g_fuel_comp_coolant_pct = s16_lohi(f.d[6], f.d[7]) * 0.1f;
  }
  else if (f.id == ID_PE15 && f.dlc >= 4) {
    g_fuel_comp_baro_pct = s16_lohi(f.d[0], f.d[1]) * 0.1f;
    g_fuel_comp_map_pct  = s16_lohi(f.d[2], f.d[3]) * 0.1f;
  }
  else if (f.id == ID_PE16 && f.dlc >= 8) {
    g_ign_comp_air_deg     = s16_lohi(f.d[0], f.d[1]) * 0.1f;
    g_ign_comp_coolant_deg = s16_lohi(f.d[2], f.d[3]) * 0.1f;
    g_ign_comp_baro_deg    = s16_lohi(f.d[4], f.d[5]) * 0.1f;
    g_ign_comp_map_deg     = s16_lohi(f.d[6], f.d[7]) * 0.1f;
  }
}

void sendTelemetryJson() {
  if (!g_seen_any_frame) return;

  g_pkt_counter++;
  uint32_t ts = millis();

  Serial.print('{');
  bool first = true;
  auto add_kv = [&](const char *key) {
    if (!first) Serial.print(',');
    first = false;
    Serial.print('"');
    Serial.print(key);
    Serial.print('"');
    Serial.print(':');
  };

  add_kv("ts_ms");   Serial.print(ts);
  add_kv("pkt");     Serial.print(g_pkt_counter);
  add_kv("src");     Serial.print("\"can\"");
  add_kv("node_id"); Serial.print(1);

  add_kv("rpm");      Serial.print(g_rpm);
  add_kv("tps_pct");  Serial.print(g_tps_pct, 1);
  add_kv("fot_ms");   Serial.print(g_fot_ms, 1);
  add_kv("ign_deg");  Serial.print(g_ign_deg, 1);

  add_kv("baro_kpa"); Serial.print(g_baro_kpa, 2);
  add_kv("map_kpa");  Serial.print(g_map_kpa, 2);
  add_kv("lambda");   Serial.print(g_lambda, 3);
  add_kv("lambda2");  Serial.print(g_lambda2, 3);
  add_kv("lambda_target"); Serial.print(g_lambda_target, 3);

  add_kv("batt_v");     Serial.print(g_batt_v, 2);
  add_kv("coolant_c");  Serial.print(g_coolant_c, 1);
  add_kv("air_c");      Serial.print(g_air_c, 1);
  add_kv("oil_psi");    Serial.print(g_oil_psi, 1);

  add_kv("ws_fl_hz"); Serial.print(g_ws_fl_hz, 1);
  add_kv("ws_fr_hz"); Serial.print(g_ws_fr_hz, 1);
  add_kv("ws_bl_hz"); Serial.print(g_ws_bl_hz, 1);
  add_kv("ws_br_hz"); Serial.print(g_ws_br_hz, 1);

  for (int i = 0; i < 8; ++i) {
    char key[12];
    snprintf(key, sizeof(key), "ai%d_v", i + 1);
    add_kv(key); Serial.print(g_ai_v[i], 3);
  }

  add_kv("therm5_temp"); Serial.print(g_therm5_temp, 1);
  add_kv("therm7_temp"); Serial.print(g_therm7_temp, 1);

  add_kv("rpm_rate_rps");    Serial.print(g_rpm_rate, 1);
  add_kv("tps_rate_pct_s"); Serial.print(g_tps_rate, 1);
  add_kv("map_rate");       Serial.print(g_map_rate, 1);
  add_kv("maf_load_rate");  Serial.print(g_maf_load_rate, 1);

  for (int i = 0; i < 8; ++i) {
    char key[18];
    snprintf(key, sizeof(key), "pwm_duty_pct_%d", i + 1);
    add_kv(key); Serial.print(g_pwm_duty_pct[i], 1);
  }

  add_kv("percent_slip");        Serial.print(g_percent_slip, 1);
  add_kv("driven_wheel_roc");    Serial.print(g_driven_wheel_roc_ft_s2, 1);
  add_kv("traction_desired_pct"); Serial.print(g_traction_desired_pct, 1);
  add_kv("driven_avg_ws_ft_s");   Serial.print(g_driven_avg_ws_ft_s, 1);
  add_kv("nondriven_avg_ws_ft_s"); Serial.print(g_nondriven_avg_ws_ft_s, 1);
  add_kv("ign_comp_deg");        Serial.print(g_ign_comp_deg, 1);
  add_kv("ign_cut_pct");         Serial.print(g_ign_cut_pct, 1);

  add_kv("driven_ws1_ft_s");    Serial.print(g_driven_ws1_ft_s, 1);
  add_kv("driven_ws2_ft_s");    Serial.print(g_driven_ws2_ft_s, 1);
  add_kv("nondriven_ws1_ft_s"); Serial.print(g_nondriven_ws1_ft_s, 1);
  add_kv("nondriven_ws2_ft_s"); Serial.print(g_nondriven_ws2_ft_s, 1);

  add_kv("fuel_comp_accel_pct");   Serial.print(g_fuel_comp_accel_pct, 1);
  add_kv("fuel_comp_start_pct");   Serial.print(g_fuel_comp_start_pct, 1);
  add_kv("fuel_comp_air_pct");     Serial.print(g_fuel_comp_air_pct, 1);
  add_kv("fuel_comp_coolant_pct"); Serial.print(g_fuel_comp_coolant_pct, 1);
  add_kv("fuel_comp_baro_pct");    Serial.print(g_fuel_comp_baro_pct, 1);
  add_kv("fuel_comp_map_pct");     Serial.print(g_fuel_comp_map_pct, 1);

  add_kv("ign_comp_air_deg");     Serial.print(g_ign_comp_air_deg, 1);
  add_kv("ign_comp_coolant_deg"); Serial.print(g_ign_comp_coolant_deg, 1);
  add_kv("ign_comp_baro_deg");    Serial.print(g_ign_comp_baro_deg, 1);
  add_kv("ign_comp_map_deg");     Serial.print(g_ign_comp_map_deg, 1);

  Serial.println('}');
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
  } else {
    Serial.print("ERR unknown cmd: ");
    Serial.println(cmd);
  }
}

uint32_t lastBlink = 0;
bool     ledState  = false;
uint32_t lastStat  = 0;
uint32_t lastJson  = 0;

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  Serial.begin(115200);
  delay(800);

  Serial.println("# RP2040 + MCP2515 dyno bridge starting");
  Serial.println("# Output: NDJSON ~20 Hz. '#' lines = status.");
  logPinout();

  pinMode(PIN_CAN_CS, OUTPUT);
  csHigh();

  if (PIN_CAN_STANDBY >= 0) {
    pinMode(PIN_CAN_STANDBY, OUTPUT);
    digitalWrite(PIN_CAN_STANDBY, LOW); // LOW = normal on most transceivers
  }
  if (PIN_CAN_RESET >= 0) {
    pinMode(PIN_CAN_RESET, OUTPUT);
    digitalWrite(PIN_CAN_RESET, HIGH);
    delay(5);
    digitalWrite(PIN_CAN_RESET, LOW);
    delay(5);
    digitalWrite(PIN_CAN_RESET, HIGH);
  }
  if (PIN_CAN_INT >= 0) {
    pinMode(PIN_CAN_INT, INPUT_PULLUP);
  }

  CAN_SPI.begin();

  CAN_SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  bool ok = mcpInit250k_16MHz();
  CAN_SPI.endTransaction();

  Serial.print("# MCP2515 init 250k/16MHz => ");
  Serial.println(ok ? "OK" : "FAIL");
  Serial.print("# CANSTAT=0x");
  Serial.println(mcpRead(CANSTAT), HEX);

  if (!ok) {
    hardFail(F("# ERROR: MCP2515 init failed. Check wiring / CS conflicts."));
  }

#if USE_CAN_INTERRUPT
  if (PIN_CAN_INT < 0) {
    Serial.println("# WARN: USE_CAN_INTERRUPT=1 but PIN_CAN_INT < 0. Falling back to polling.");
  } else {
    attachInterrupt(digitalPinToInterrupt(PIN_CAN_INT), [] {
      // Nothing needed here—just wake the MCU. Actual reads happen in loop().
    }, FALLING);
  }
#endif
}

void loop() {
  uint32_t now = millis();

  while (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() > 0) handleCommand(cmd);
  }

  if (now - lastBlink > 500) {
    lastBlink = now;
    ledState = !ledState;
    digitalWrite(LED_BUILTIN, ledState);
  }

  if (now - lastStat > 1000) {
    lastStat = now;
    uint8_t intf = mcpRead(CANINTF);
    uint8_t eflg = mcpRead(EFLG);
    uint8_t tec  = mcpRead(TEC);
    uint8_t rec  = mcpRead(REC);

    Serial.print("# STAT CANINTF=0x"); Serial.print(intf, HEX);
    Serial.print(" EFLG=0x");         Serial.print(eflg, HEX);
    Serial.print(" TEC=");             Serial.print(tec);
    Serial.print(" REC=");             Serial.println(rec);

    if (eflg & 0x60) {
      mcpBitMod(EFLG, 0x60, 0x00);
      Serial.println("# cleared RX overflow flags");
    }
  }

  uint8_t intf = mcpRead(CANINTF);
  Frame f{};

  if (intf & RX0IF) {
    readRx(RXB0SIDH, f);
    mcpBitMod(CANINTF, RX0IF, 0x00);
    printFrameDebug(f);
    updateTelemetryFromFrame(f);
  }

  if (intf & RX1IF) {
    readRx(RXB1SIDH, f);
    mcpBitMod(CANINTF, RX1IF, 0.00);
    printFrameDebug(f);
    updateTelemetryFromFrame(f);
  }

  if (now - lastJson >= 50) {
    lastJson = now;
    sendTelemetryJson();
  }
}
