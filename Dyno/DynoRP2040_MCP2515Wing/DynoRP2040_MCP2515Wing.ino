// DynoRP2040_MCP2515Wing.ino
//
// Adafruit Feather RP2040 + FeatherWing MCP2515 (SPI) stack
//   → Sniffs PE3 Bosch MS4.3 CAN frames (per the PE3 8400 DBC) at 500 kbps
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
// Set to 1 to print every CAN frame (very chatty; can corrupt JSON at 115200).
#define CAN_DEBUG 0

SPIClassRP2040 CAN_SPI(spi1, PIN_CAN_MISO, PIN_CAN_CS, PIN_CAN_SCK, PIN_CAN_MOSI);

#ifndef MCP_TIMING_PRESET
#define MCP_TIMING_PRESET 1
#endif

struct CanTiming {
  uint8_t cnf1;
  uint8_t cnf2;
  uint8_t cnf3;
  const char *label;
};

static constexpr CanTiming kTimingPresets[] = {
  {0x00, 0x90, 0x02, "BRP0 sample~75%"},
  {0x01, 0x90, 0x02, "BRP1 sample~70%"}
};

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

// Bit timing for Bosch PE3 bus: 500 kbps @ 16 MHz crystal on the FeatherWing
bool mcpInit500k_16MHz() {
  mcpReset();
  if (!mcpSetMode(MODE_CONFIG)) return false;

  constexpr size_t timingCount = sizeof(kTimingPresets) / sizeof(kTimingPresets[0]);
  const CanTiming &timing = kTimingPresets[MCP_TIMING_PRESET % timingCount];

  mcpWrite(CNF1, timing.cnf1);
  mcpWrite(CNF2, timing.cnf2);
  mcpWrite(CNF3, timing.cnf3);

  mcpWrite(RXB0CTRL, 0x60); // receive-any
  mcpWrite(RXB1CTRL, 0x60);
  mcpWrite(CANINTE, RX0IF | RX1IF);
  mcpWrite(CANINTF, 0x00);

  bool ok = mcpSetMode(MODE_NORMAL);
  Serial.print("# MCP2515 timing preset ");
  Serial.print(MCP_TIMING_PRESET);
  Serial.print(" (");
  Serial.print(timing.label);
  Serial.println(ok ? ") => Normal" : ") => FAIL");
  return ok;
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
static float    g_veh_kph         = 0.0f;
static float    g_tps_pct         = 0.0f;
static float    g_ign_deg         = 0.0f;

static float    g_map_kpa         = 0.0f;
static float    g_map_ext_kpa     = 0.0f;
static float    g_lambda          = 0.0f;
static float    g_lambda2         = 0.0f;

static float    g_batt_v          = 0.0f;
static float    g_coolant_c       = 0.0f;
static float    g_oil_temp_c      = 0.0f;
static float    g_air_c           = 0.0f;
static uint8_t  g_gear            = 0;

static float    g_fuel_bar        = 0.0f;
static float    g_fuel_psi        = 0.0f;
static float    g_oil_bar         = 0.0f;
static float    g_oil_psi         = 0.0f;

// Helpers
static inline uint16_t u16_lohi(uint8_t lo, uint8_t hi) {
  return (uint16_t)hi << 8 | lo;
}

static inline int16_t s16_lohi(uint8_t lo, uint8_t hi) {
  uint16_t n = u16_lohi(lo, hi);
  return (n > 32767) ? (int16_t)(n - 65536) : (int16_t)n;
}

static constexpr float BAR_TO_PSI = 14.503774f;
static constexpr float PSI_TO_KPA = 6.894757f;

static const uint32_t ID_PE3_770 = 0x770;
static const uint32_t ID_PE3_771 = 0x771;
static const uint32_t ID_PE3_772 = 0x772;
static const uint32_t ID_PE3_790 = 0x790;

void updateTelemetryFromFrame(const Frame &f) {
  if (f.ext) return;

  if (f.id == ID_PE3_770 && f.dlc >= 7) {
    g_seen_any_frame = true;
    uint8_t row = f.d[0];
    uint16_t raw_rpm = u16_lohi(f.d[1], f.d[2]);
    g_rpm = (uint16_t)((raw_rpm * 0.25f) + 0.5f);
    g_veh_kph = u16_lohi(f.d[3], f.d[4]) * 0.00781f;
    g_tps_pct = f.d[5] * 0.391f;
    g_ign_deg = (int8_t)f.d[6] * 1.365f;

    if (f.dlc >= 8) {
      if (row == 3) {
        g_coolant_c = f.d[7] - 40.0f;
      } else if (row == 4) {
        g_oil_temp_c = f.d[7] - 39.0f;
      } else if (row == 6) {
        g_air_c = f.d[7] - 40.0f;
      } else if (row == 9) {
        g_gear = f.d[7];
      }
    }
  }
  else if (f.id == ID_PE3_771 && f.dlc >= 6) {
    g_seen_any_frame = true;
    uint8_t row = f.d[0];
    if (row == 0 && f.dlc >= 7) {
      g_map_kpa = u16_lohi(f.d[5], f.d[6]) * 0.01f;
    } else if (row == 1 && f.dlc >= 8) {
      g_batt_v = f.d[7] * 0.111f;
    } else if (row == 3 && f.dlc >= 8) {
      g_fuel_bar = f.d[5] * 0.0515f;
      g_oil_bar  = f.d[7] * 0.0513f;
      g_fuel_psi = g_fuel_bar * BAR_TO_PSI;
      g_oil_psi  = g_oil_bar  * BAR_TO_PSI;
    }
  }
  else if (f.id == ID_PE3_772 && f.dlc >= 6) {
    if (f.d[0] == 0) {
      g_seen_any_frame = true;
      g_lambda  = u16_lohi(f.d[2], f.d[3]) * 0.000244f;
      g_lambda2 = u16_lohi(f.d[4], f.d[5]) * 0.000244f;
    }
  }
  else if (f.id == ID_PE3_790) {
    g_seen_any_frame = true;
    if (f.dlc >= 2) {
      g_rpm = u16_lohi(f.d[0], f.d[1]);
    }
    if (f.dlc >= 6) {
      g_ign_deg = u16_lohi(f.d[4], f.d[5]) * 0.1f;
    }
    if (f.dlc >= 8) {
      g_map_ext_kpa = u16_lohi(f.d[6], f.d[7]) * 0.01f * PSI_TO_KPA;
    }
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

  add_kv("rpm");            Serial.print(g_rpm);
  add_kv("veh_kph");        Serial.print(g_veh_kph, 2);
  add_kv("tps_pct");        Serial.print(g_tps_pct, 1);
  add_kv("ign_deg");        Serial.print(g_ign_deg, 1);
  add_kv("map_kpa");        Serial.print(g_map_kpa, 2);
  add_kv("map_ext_kpa");    Serial.print(g_map_ext_kpa, 2);
  add_kv("lambda");         Serial.print(g_lambda, 3);
  add_kv("lambda2");        Serial.print(g_lambda2, 3);
  add_kv("batt_v");         Serial.print(g_batt_v, 2);
  add_kv("coolant_c");      Serial.print(g_coolant_c, 1);
  add_kv("oil_temp_c");     Serial.print(g_oil_temp_c, 1);
  add_kv("air_c");          Serial.print(g_air_c, 1);
  add_kv("gear");           Serial.print(g_gear);
  add_kv("fuel_bar");       Serial.print(g_fuel_bar, 2);
  add_kv("fuel_psi");       Serial.print(g_fuel_psi, 2);
  add_kv("oil_bar");        Serial.print(g_oil_bar, 2);
  add_kv("oil_psi");        Serial.print(g_oil_psi, 2);

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
  bool ok = mcpInit500k_16MHz();
  CAN_SPI.endTransaction();

  Serial.print("# MCP2515 init 500k/16MHz => ");
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

    if (CAN_DEBUG) {
      Serial.print("# STAT CANINTF=0x"); Serial.print(intf, HEX);
      Serial.print(" EFLG=0x");         Serial.print(eflg, HEX);
      Serial.print(" TEC=");             Serial.print(tec);
      Serial.print(" REC=");             Serial.println(rec);
    }

    if (eflg & 0x60) {
      mcpBitMod(EFLG, 0x60, 0x00);
      if (CAN_DEBUG) {
        Serial.println("# cleared RX overflow flags");
      }
    }
  }

  uint8_t intf = mcpRead(CANINTF);
  Frame f{};

  if (intf & RX0IF) {
    readRx(RXB0SIDH, f);
    mcpBitMod(CANINTF, RX0IF, 0x00);
    if (CAN_DEBUG) {
      printFrameDebug(f);
    }
    updateTelemetryFromFrame(f);
  }

  if (intf & RX1IF) {
    readRx(RXB1SIDH, f);
    mcpBitMod(CANINTF, RX1IF, 0.00);
    if (CAN_DEBUG) {
      printFrameDebug(f);
    }
    updateTelemetryFromFrame(f);
  }

  if (now - lastJson >= 50) {
    lastJson = now;
    sendTelemetryJson();
  }
}
