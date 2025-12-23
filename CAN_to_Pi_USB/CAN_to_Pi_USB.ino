// CAN_to_Pi_USB.ino
//
// Feather RP2040 + MCP2515
//   → Sniff PE3 CAN frames (AN400 protocol)
//   → Stream raw frames over USB serial to Raspberry Pi.
//
// This sketch combines the low‑level MCP2515 CAN receiver from Test1.ino
// with the simple USB serial telemetry pattern from Communicationtest.ino.
//
// Serial line format to the Pi (one line per received CAN frame):
//   CAN,<ts_ms>,<id_hex>,<ext>,<dlc>,<b0>,<b1>,<b2>,<b3>,<b4>,<b5>,<b6>,<b7>
//
//  - ts_ms:   millis() timestamp (unsigned long)
//  - id_hex:  arbitration ID in hex (no 0x prefix), e.g. CFFF048
//  - ext:     1 for extended frame, 0 for standard
//  - dlc:     Data length code (0–8)
//  - b0..b7:  data bytes as unsigned decimal 0–255 (unused bytes = 0)
//
// Any other Serial output (e.g. human‑readable debug) does NOT start with
// "CAN," so the Pi reader can ignore those lines.

#include <Arduino.h>
#include <SPI.h>

// Feather RP2040 SPI pins used in Test1.ino
static constexpr uint8_t PIN_MISO = 8;
static constexpr uint8_t PIN_SCK  = 14;
static constexpr uint8_t PIN_MOSI = 15;

// MCP2515 CS
static constexpr uint8_t CAN_CS   = 5;

SPIClassRP2040 CAN_SPI(spi1, PIN_MISO, CAN_CS, PIN_SCK, PIN_MOSI);

// MCP2515 commands
#define CMD_RESET   0xC0
#define CMD_READ    0x03
#define CMD_WRITE   0x02
#define CMD_BITMOD  0x05

// Registers
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

// RX buffer bases
#define RXB0SIDH 0x61
#define RXB1SIDH 0x71

// Flags
#define RX0IF 0x01
#define RX1IF 0x02

// Modes
#define MODE_MASK   0xE0
#define MODE_CONFIG 0x80
#define MODE_NORMAL 0x00

struct Frame {
  uint32_t id;
  bool     ext;
  uint8_t  dlc;
  uint8_t  d[8];
};

static inline void csLow()  { digitalWrite(CAN_CS, LOW); }
static inline void csHigh() { digitalWrite(CAN_CS, HIGH); }

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

// 250 kbps @ 16 MHz (matches Test1.ino and AN400 spec)
bool mcpInit250k_16MHz() {
  mcpReset();

  if (!mcpSetMode(MODE_CONFIG)) return false;

  // CNF1/2/3 = {0x41, 0xF1, 0x85}
  mcpWrite(CNF1, 0x41);
  mcpWrite(CNF2, 0xF1);
  mcpWrite(CNF3, 0x85);

  // Receive-any
  mcpWrite(RXB0CTRL, 0x60);
  mcpWrite(RXB1CTRL, 0x60);

  // Enable RX interrupts
  mcpWrite(CANINTE, RX0IF | RX1IF);

  // Clear flags
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

  // Zero fill, then read only dlc bytes
  for (uint8_t i = 0; i < 8; i++) {
    f.d[i] = 0;
  }
  for (uint8_t i = 0; i < dlc && i < 8; i++) {
    f.d[i] = mcpRead(base + 5 + i);
  }
  return true;
}

void printFrameDebug(const Frame &f) {
  // Human‑readable debug, ignored by Pi parser.
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
// ===== Telemetry decoding and JSON output =====

// Global telemetry snapshot (latest values seen on CAN)
static uint32_t g_pkt_counter     = 0;
static bool     g_seen_any_frame  = false;

static uint16_t g_rpm             = 0;
static float    g_tps_pct         = 0.0f;
static float    g_fot_ms          = 0.0f;
static float    g_ign_deg         = 0.0f;

static float    g_baro_kpa        = 0.0f;
static float    g_map_kpa         = 0.0f;
static float    g_lambda          = 0.0f;

static float    g_oil_psi         = 0.0f;
static float    g_batt_v          = 0.0f;
static float    g_coolant_c       = 0.0f;
static float    g_air_c           = 0.0f;

static float    g_ws_fl_hz        = 0.0f;
static float    g_ws_fr_hz        = 0.0f;
static float    g_ws_bl_hz        = 0.0f;
static float    g_ws_br_hz        = 0.0f;

// Simple helpers from Test1.ino / AN400
static inline uint16_t u16_lohi(uint8_t lo, uint8_t hi) {
  return (uint16_t)hi << 8 | lo;
}

static inline int16_t s16_lohi(uint8_t lo, uint8_t hi) {
  uint16_t n = u16_lohi(lo, hi);
  if (n > 32767) return (int16_t)(n - 65536);
  return (int16_t)n;
}

// Known PE IDs from AN400
static const uint32_t ID_PE1 = 0x0CFFF048; // RPM, TPS, Fuel Open Time, Ign Angle
static const uint32_t ID_PE2 = 0x0CFFF148; // Barometer, MAP, Lambda, Pressure Type
static const uint32_t ID_PE3 = 0x0CFFF248; // Project‑specific: Oil pressure etc.
static const uint32_t ID_PE5 = 0x0CFFF448; // Wheel speeds
static const uint32_t ID_PE6 = 0x0CFFF548; // Battery, Air Temp, Coolant
static const uint32_t ID_PE9 = 0x0CFFF848; // Lambda & AFR

void updateTelemetryFromFrame(const Frame &f) {
  if (!f.ext) return; // AN400 frames are extended

  g_seen_any_frame = true;

  if (f.id == ID_PE1 && f.dlc >= 8) {
    g_rpm     = u16_lohi(f.d[0], f.d[1]);        // 1 rpm/bit
    g_tps_pct = s16_lohi(f.d[2], f.d[3]) * 0.1f; // %
    g_fot_ms  = s16_lohi(f.d[4], f.d[5]) * 0.1f; // ms
    g_ign_deg = s16_lohi(f.d[6], f.d[7]) * 0.1f; // deg
  }
  else if (f.id == ID_PE2 && f.dlc >= 8) {
    float baro_raw = s16_lohi(f.d[0], f.d[1]) * 0.01f; // psi or kPa
    float map_raw  = s16_lohi(f.d[2], f.d[3]) * 0.01f; // psi or kPa
    float lam_raw  = s16_lohi(f.d[4], f.d[5]) * 0.01f; // lambda
    bool kpa       = (f.d[6] & 0x01) != 0;             // pressure type bit

    if (kpa) {
      g_baro_kpa = baro_raw;
      g_map_kpa  = map_raw;
    } else {
      const float PSI_TO_KPA = 6.89476f;
      g_baro_kpa = baro_raw * PSI_TO_KPA;
      g_map_kpa  = map_raw * PSI_TO_KPA;
    }
    g_lambda = lam_raw;
  }
  else if (f.id == ID_PE3 && f.dlc >= 8) {
    // Project‑specific mapping from your 2025 dashboard data_parser:
    // Oil pressure encoded in bytes 2–3 with custom linear conversion.
    int16_t raw_op = s16_lohi(f.d[2], f.d[3]);
    g_oil_psi = (raw_op / 1000.0f) * 25.0f - 12.5f;
  }
  else if (f.id == ID_PE5 && f.dlc >= 8) {
    // Wheel speeds in Hz (0.2 Hz/bit)
    g_ws_fr_hz = s16_lohi(f.d[0], f.d[1]) * 0.2f;
    g_ws_fl_hz = s16_lohi(f.d[2], f.d[3]) * 0.2f;
    g_ws_br_hz = s16_lohi(f.d[4], f.d[5]) * 0.2f;
    g_ws_bl_hz = s16_lohi(f.d[6], f.d[7]) * 0.2f;
  }
  else if (f.id == ID_PE6 && f.dlc >= 8) {
    float batt_v  = s16_lohi(f.d[0], f.d[1]) * 0.01f; // volts
    float air_raw = s16_lohi(f.d[2], f.d[3]) * 0.1f;  // C or F
    float clt_raw = s16_lohi(f.d[4], f.d[5]) * 0.1f;  // C or F
    bool temp_c   = (f.d[6] & 0x01) != 0;             // 0=F, 1=C per AN400

    g_batt_v = batt_v;

    if (temp_c) {
      g_air_c     = air_raw;
      g_coolant_c = clt_raw;
    } else {
      // F → C conversion
      g_air_c     = (air_raw - 32.0f) * (5.0f / 9.0f);
      g_coolant_c = (clt_raw - 32.0f) * (5.0f / 9.0f);
    }
  }
  else if (f.id == ID_PE9 && f.dlc >= 8) {
    g_lambda = s16_lohi(f.d[0], f.d[1]) * 0.01f; // lambda #1 measured
    // Other lambda fields available if you need them later
  }
}

// Emit one line of NDJSON telemetry representing the latest snapshot.
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
  add_kv("node_id"); Serial.print(1); // hard‑coded node id for now

  // Engine basic
  add_kv("rpm");      Serial.print(g_rpm);
  add_kv("tps_pct");  Serial.print(g_tps_pct, 1);
  add_kv("fot_ms");   Serial.print(g_fot_ms, 1);
  add_kv("ign_deg");  Serial.print(g_ign_deg, 1);

  // Pressure & lambda
  add_kv("baro_kpa"); Serial.print(g_baro_kpa, 2);
  add_kv("map_kpa");  Serial.print(g_map_kpa, 2);
  add_kv("lambda");   Serial.print(g_lambda, 3);

  // Temps & voltage & oil
  add_kv("batt_v");     Serial.print(g_batt_v, 2);
  add_kv("coolant_c");  Serial.print(g_coolant_c, 1);
  add_kv("air_c");      Serial.print(g_air_c, 1);
  add_kv("oil_psi");    Serial.print(g_oil_psi, 1);

  // Wheel speeds
  add_kv("ws_fl_hz"); Serial.print(g_ws_fl_hz, 1);
  add_kv("ws_fr_hz"); Serial.print(g_ws_fr_hz, 1);
  add_kv("ws_bl_hz"); Serial.print(g_ws_bl_hz, 1);
  add_kv("ws_br_hz"); Serial.print(g_ws_br_hz, 1);

  Serial.println('}');
}

// Optional: simple command handler over USB (from Communicationtest.ino)
void handleCommand(const String &cmd) {
  if (cmd == "PING") {
    Serial.println("PONG");
  } else if (cmd == "LED ON") {
    digitalWrite(LED_BUILTIN, HIGH);
    Serial.println("OK LED ON");
  } else if (cmd == "LED OFF") {
    digitalWrite(LED_BUILTIN, LOW);
    Serial.println("OK LED OFF");
  } else if (cmd.startsWith("RATE ")) {
    // Placeholder: could be used later to throttle Serial output
    Serial.print("OK ");
    Serial.println(cmd);
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

  Serial.println("# RP2040 CAN→USB bridge starting");
  Serial.println("# Output: one JSON telemetry object per line (NDJSON)");

  pinMode(CAN_CS, OUTPUT);
  csHigh();

  CAN_SPI.begin();

  CAN_SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  bool ok = mcpInit250k_16MHz();
  CAN_SPI.endTransaction();

  Serial.print("# MCP2515 init 250k/16MHz => ");
  Serial.println(ok ? "OK" : "FAIL");
  Serial.print("# CANSTAT=0x");
  Serial.println(mcpRead(CANSTAT), HEX);
}

void loop() {
  uint32_t now = millis();

  // ---- Read commands from Pi ----
  while (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() > 0) handleCommand(cmd);
  }

  // ---- Status / heartbeat ----
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

    // Clear overflow flags if present (RX0OVR=0x20, RX1OVR=0x40)
    if (eflg & 0x60) {
      mcpBitMod(EFLG, 0x60, 0x00);
      Serial.println("# cleared RX overflow flags");
    }
  }

  // ---- Poll for RX flags and read buffers ----
  uint8_t intf = mcpRead(CANINTF);
  Frame f{};  // zero‑init

  if (intf & RX0IF) {
    readRx(RXB0SIDH, f);
    mcpBitMod(CANINTF, RX0IF, 0x00); // clear RX0IF
    printFrameDebug(f);
    updateTelemetryFromFrame(f);
  }

  if (intf & RX1IF) {
    readRx(RXB1SIDH, f);
    mcpBitMod(CANINTF, RX1IF, 0x00); // clear RX1IF
    printFrameDebug(f);
    updateTelemetryFromFrame(f);
  }

  // ---- Periodic JSON telemetry snapshot (e.g. 10 Hz) ----
  if (now - lastJson >= 100) { // 100 ms
    lastJson = now;
    sendTelemetryJson();
  }
}
