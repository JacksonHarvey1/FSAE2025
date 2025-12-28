// CarCanLoRaTx.ino
//
// Feather RP2040 + CAN wing (MCP2515 on SPI1) + built-in RFM95 (LoRa)
//
// Role: Read AN400 CAN frames from the ECU, maintain a decoded telemetry
// snapshot, and transmit a compact *binary* packet over LoRa at ~20 Hz.
//
// Binary packet layout (little endian, 22 bytes):
//   Byte 0-1  : uint16  pkt      (packet counter, wraps at 65535)
//   Byte 2-3  : uint16  rpm      (1 rpm / bit)
//   Byte 4-5  : uint16  map_raw  (map_kpa * 10, 0.1 kPa / bit)
//   Byte 6-7  : uint16  tps_raw  (tps_pct * 10, 0.1 % / bit)
//   Byte 8-9  : int16   oil_raw  (oil_psi * 10, 0.1 psi / bit)
//   Byte 10-11: int16   clt_raw  (coolant_c * 10, 0.1 °C / bit)
//   Byte 12   : uint8   lam_raw  (lambda * 100, 0.01 / bit, 0–2.55)
//   Byte 13   : uint8   batt_raw (batt_v * 10, 0.1 V / bit, 0–25.5 V)
//   Byte 14-15: uint16  ws_fl    (ws_fl_hz * 2, 0.5 Hz / bit)
//   Byte 16-17: uint16  ws_fr    (ws_fr_hz * 2)
//   Byte 18-19: uint16  ws_bl    (ws_bl_hz * 2)
//   Byte 20-21: uint16  ws_br    (ws_br_hz * 2)
//
// The pit-side LoRa RX node unpacks this and reconstructs JSON with fields
// matching the dyno CAN→USB JSON schema, then streams NDJSON to the Pi.

#include <Arduino.h>
#include <SPI.h>
#include <RH_RF95.h>

// -------------------- LoRa radio config (built-in RFM95) --------------------

#define RFM95_CS   16
#define RFM95_INT  21
#define RFM95_RST  17

// Match this to your region (915 MHz for US)
#define RF95_FREQ  915.0

RH_RF95 rf95(RFM95_CS, RFM95_INT);

// -------------------- MCP2515 / CAN on SPI1 (same as CAN_to_Pi_USB) --------

// Feather RP2040 SPI1 pins (from CAN_to_Pi_USB.ino)
static constexpr uint8_t PIN_MISO = 8;
static constexpr uint8_t PIN_SCK  = 14;
static constexpr uint8_t PIN_MOSI = 15;

// MCP2515 CS
static constexpr uint8_t CAN_CS   = 5;

SPIClassRP2040 CAN_SPI(spi1, PIN_MISO, CAN_CS, PIN_SCK, PIN_MOSI);

// MCP2515 commands / registers (copied from CAN_to_Pi_USB.ino)
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

// 250 kbps @ 16 MHz (AN400 spec)
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

  for (uint8_t i = 0; i < 8; i++) {
    f.d[i] = 0;
  }
  for (uint8_t i = 0; i < dlc && i < 8; i++) {
    f.d[i] = mcpRead(base + 5 + i);
  }
  return true;
}

void printFrameDebug(const Frame &f) {
  Serial.print("# CAN RX ");
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

// -------------------- Telemetry decoding (from CAN_to_Pi_USB) --------------

static uint16_t g_rpm       = 0;
static float    g_tps_pct   = 0.0f;
static float    g_fot_ms    = 0.0f;
static float    g_ign_deg   = 0.0f;

static float    g_baro_kpa  = 0.0f;
static float    g_map_kpa   = 0.0f;
static float    g_lambda    = 0.0f;

static float    g_oil_psi   = 0.0f;
static float    g_batt_v    = 0.0f;
static float    g_coolant_c = 0.0f;
static float    g_air_c     = 0.0f;

static float    g_ws_fl_hz  = 0.0f;
static float    g_ws_fr_hz  = 0.0f;
static float    g_ws_bl_hz  = 0.0f;
static float    g_ws_br_hz  = 0.0f;

static bool     g_seen_any_frame = false;

static inline uint16_t u16_lohi(uint8_t lo, uint8_t hi) {
  return (uint16_t)hi << 8 | lo;
}

static inline int16_t s16_lohi(uint8_t lo, uint8_t hi) {
  uint16_t n = u16_lohi(lo, hi);
  if (n > 32767) return (int16_t)(n - 65536);
  return (int16_t)n;
}

// Known PE IDs from AN400 (same as CAN_to_Pi_USB.ino)
static const uint32_t ID_PE1 = 0x0CFFF048; // RPM, TPS, Fuel Open Time, Ign Angle
static const uint32_t ID_PE2 = 0x0CFFF148; // Barometer, MAP, Lambda, Pressure Type
static const uint32_t ID_PE3 = 0x0CFFF248; // Project-specific: Oil pressure etc.
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
    int16_t raw_op = s16_lohi(f.d[2], f.d[3]);
    g_oil_psi = (raw_op / 1000.0f) * 25.0f - 12.5f;
  }
  else if (f.id == ID_PE5 && f.dlc >= 8) {
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
      g_air_c     = (air_raw - 32.0f) * (5.0f / 9.0f);
      g_coolant_c = (clt_raw - 32.0f) * (5.0f / 9.0f);
    }
  }
  else if (f.id == ID_PE9 && f.dlc >= 8) {
    g_lambda = s16_lohi(f.d[0], f.d[1]) * 0.01f; // lambda #1 measured
  }
}

// -------------------- LoRa packet encoder ----------------------------------

static uint16_t g_lora_pkt_counter = 0;

static inline uint16_t clamp_u16(float v) {
  if (v < 0.0f) return 0;
  if (v > 65535.0f) return 65535;
  return (uint16_t)(v + 0.5f);
}

static inline int16_t clamp_i16(float v) {
  if (v < -32768.0f) return -32768;
  if (v > 32767.0f)  return 32767;
  return (int16_t)(v + (v >= 0 ? 0.5f : -0.5f));
}

void write_u16_le(uint8_t *buf, int idx, uint16_t v) {
  buf[idx + 0] = (uint8_t)(v & 0xFF);
  buf[idx + 1] = (uint8_t)(v >> 8);
}

void write_i16_le(uint8_t *buf, int idx, int16_t v) {
  buf[idx + 0] = (uint8_t)(v & 0xFF);
  buf[idx + 1] = (uint8_t)((uint16_t)v >> 8);
}

void sendLoRaTelemetry() {
  if (!g_seen_any_frame) return;  // Don't spam until we've seen valid CAN

  uint8_t payload[22];

  uint16_t pkt = g_lora_pkt_counter++;

  // Quantize using the layout documented at top of file
  uint16_t rpm      = g_rpm;                        // 1 rpm/bit
  uint16_t map_raw  = clamp_u16(g_map_kpa * 10.0f); // 0.1 kPa/bit
  uint16_t tps_raw  = clamp_u16(g_tps_pct * 10.0f); // 0.1 %/bit
  int16_t  oil_raw  = clamp_i16(g_oil_psi * 10.0f); // 0.1 psi/bit
  int16_t  clt_raw  = clamp_i16(g_coolant_c * 10.0f); // 0.1 °C/bit
  uint8_t  lam_raw  = (uint8_t)constrain((int)(g_lambda * 100.0f + 0.5f), 0, 255);
  uint8_t  batt_raw = (uint8_t)constrain((int)(g_batt_v * 10.0f + 0.5f), 0, 255);

  uint16_t ws_fl = clamp_u16(g_ws_fl_hz * 2.0f); // 0.5 Hz/bit
  uint16_t ws_fr = clamp_u16(g_ws_fr_hz * 2.0f);
  uint16_t ws_bl = clamp_u16(g_ws_bl_hz * 2.0f);
  uint16_t ws_br = clamp_u16(g_ws_br_hz * 2.0f);

  write_u16_le(payload, 0,  pkt);
  write_u16_le(payload, 2,  rpm);
  write_u16_le(payload, 4,  map_raw);
  write_u16_le(payload, 6,  tps_raw);
  write_i16_le(payload, 8,  oil_raw);
  write_i16_le(payload, 10, clt_raw);
  payload[12] = lam_raw;
  payload[13] = batt_raw;
  write_u16_le(payload, 14, ws_fl);
  write_u16_le(payload, 16, ws_fr);
  write_u16_le(payload, 18, ws_bl);
  write_u16_le(payload, 20, ws_br);

  rf95.send(payload, sizeof(payload));
  rf95.waitPacketSent();
}

// -------------------- Misc / status ----------------------------------------

uint32_t lastBlink   = 0;
bool     ledState    = false;
uint32_t lastStat    = 0;
uint32_t lastLoRaTx  = 0;

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  Serial.begin(115200);
  delay(500);
  Serial.println("# CarCanLoRaTx starting");

  // --- CAN / MCP2515 init on SPI1 ---
  pinMode(CAN_CS, OUTPUT);
  csHigh();

  CAN_SPI.begin();

  CAN_SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  bool can_ok = mcpInit250k_16MHz();
  CAN_SPI.endTransaction();

  Serial.print("# MCP2515 init 250k/16MHz => ");
  Serial.println(can_ok ? "OK" : "FAIL");
  Serial.print("# CANSTAT=0x");
  Serial.println(mcpRead(CANSTAT), HEX);

  // --- LoRa radio init (built-in RFM95) ---
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

  rf95.setTxPower(13, false);  // 13 dBm is a safe default

  Serial.print("# LoRa RF95 @ ");
  Serial.print(RF95_FREQ);
  Serial.println(" MHz ready");
}

void loop() {
  uint32_t now = millis();

  // Blink LED at 1 Hz to indicate life
  if (now - lastBlink >= 500) {
    lastBlink = now;
    ledState = !ledState;
    digitalWrite(LED_BUILTIN, ledState);
  }

  // Periodic CAN status
  if (now - lastStat >= 1000) {
    lastStat = now;
    uint8_t intf = mcpRead(CANINTF);
    uint8_t eflg = mcpRead(EFLG);
    uint8_t tec  = mcpRead(TEC);
    uint8_t rec  = mcpRead(REC);

    Serial.print("# CAN STAT CANINTF=0x"); Serial.print(intf, HEX);
    Serial.print(" EFLG=0x");           Serial.print(eflg, HEX);
    Serial.print(" TEC=");              Serial.print(tec);
    Serial.print(" REC=");              Serial.println(rec);

    if (eflg & 0x60) {
      mcpBitMod(EFLG, 0x60, 0x00);
      Serial.println("# cleared RX overflow flags");
    }
  }

  // Poll CAN RX buffers
  uint8_t intf = mcpRead(CANINTF);
  Frame f{}; // zero-init

  if (intf & RX0IF) {
    readRx(RXB0SIDH, f);
    mcpBitMod(CANINTF, RX0IF, 0x00);
    printFrameDebug(f);
    updateTelemetryFromFrame(f);
  }

  if (intf & RX1IF) {
    readRx(RXB1SIDH, f);
    mcpBitMod(CANINTF, RX1IF, 0x00);
    printFrameDebug(f);
    updateTelemetryFromFrame(f);
  }

  // LoRa telemetry at ~20 Hz (every 50 ms)
  if (now - lastLoRaTx >= 50) {
    lastLoRaTx = now;
    sendLoRaTelemetry();
  }
}

