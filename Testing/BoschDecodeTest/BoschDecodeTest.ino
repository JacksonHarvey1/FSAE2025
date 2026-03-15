#include <Arduino.h>
#include <SPI.h>

// Simple MCP2515 bring-up (mirrors Testing/SimpleCANTest but prints full Bosch snapshot)
static const uint8_t MCP_PIN_MISO = 8;   // Feather RP2040 spi1 MISO
static const uint8_t MCP_PIN_MOSI = 15;  // Feather RP2040 spi1 MOSI
static const uint8_t MCP_PIN_SCK  = 14;  // Feather RP2040 spi1 SCK
static const uint8_t MCP_PIN_CS   = 5;   // Dedicated CS for MCP2515 wing
static const uint8_t MCP_PIN_INT  = 6;   // Optional INT (polling by default)
static const int8_t  MCP_PIN_STANDBY = -1;
static const int8_t  MCP_PIN_RESET   = -1;

#define CMD_RESET   0xC0
#define CMD_READ    0x03
#define CMD_WRITE   0x02
#define CMD_BITMOD  0x05

#define CANSTAT   0x0E
#define CANCTRL   0x0F
#define CNF3      0x28
#define CNF2      0x29
#define CNF1      0x2A
#define CANINTE   0x2B
#define CANINTF   0x2C
#define EFLG      0x2D
#define RXB0CTRL  0x60
#define RXB1CTRL  0x70
#define RXB0SIDH  0x61
#define RXB1SIDH  0x71

#define RX0IF     0x01
#define RX1IF     0x02

#define MODE_MASK    0xE0
#define MODE_CONFIG  0x80
#define MODE_NORMAL  0x00

static const uint32_t ID_PE3_770 = 0x770;
static const uint32_t ID_PE3_771 = 0x771;
static const uint32_t ID_PE3_772 = 0x772;
static const uint32_t ID_PE3_790 = 0x790;

#ifndef CAN_LISTEN_ONLY
#define CAN_LISTEN_ONLY 0
#endif

static constexpr float BAR_TO_PSI = 14.503774f;
static constexpr float PSI_TO_KPA = 6.894757f;

#ifndef MCP_TIMING_PRESET
// 0 = BRP0 timing (CNF1=0x00, CNF2=0x90, CNF3=0x02)
// 1 = BRP1 timing (CNF1=0x01, CNF2=0x90, CNF3=0x02)
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

SPIClassRP2040 CAN_SPI(spi1, MCP_PIN_MISO, MCP_PIN_CS, MCP_PIN_SCK, MCP_PIN_MOSI);

struct Frame {
  uint32_t id;
  bool ext;
  uint8_t dlc;
  uint8_t data[8];
};

struct BoschSnapshot {
  bool seen_bosch = false;
  bool has770 = false;
  bool has771_row0 = false;
  bool has771_row1 = false;
  bool has771_row3 = false;
  bool has772 = false;
  bool has790 = false;

  float rpm = 0.0f;
  float veh_kph = 0.0f;
  float tps_pct = 0.0f;
  float ign_deg = 0.0f;
  float coolant_c = NAN;
  float oil_temp_c = NAN;
  float iat_c = NAN;
  uint8_t gear = 0;

  float map_kpa = 0.0f;
  float map_ext_kpa = 0.0f;
  float lambda1 = 0.0f;
  float lambda2 = 0.0f;

  float batt_v = 0.0f;
  float fuel_bar = 0.0f;
  float fuel_psi = 0.0f;
  float oil_bar = 0.0f;
  float oil_psi = 0.0f;
};

BoschSnapshot g_snap;

static inline void csLow()  { digitalWrite(MCP_PIN_CS, LOW); }
static inline void csHigh() { digitalWrite(MCP_PIN_CS, HIGH); }

const char *modeToString(uint8_t mode) {
  mode &= MODE_MASK;
  switch (mode) {
    case 0x00: return "Normal";
    case 0x20: return "Sleep";
    case 0x40: return "Loopback";
    case 0x60: return "ListenOnly";
    case 0x80: return "Config";
    default:   return "Unknown";
  }
}

void dumpCanRegisters(const char *tag) {
  uint8_t canstat = mcpRead(CANSTAT);
  uint8_t canctrl = mcpRead(CANCTRL);
  uint8_t cnf1 = mcpRead(CNF1);
  uint8_t cnf2 = mcpRead(CNF2);
  uint8_t cnf3 = mcpRead(CNF3);
  uint8_t rxb0 = mcpRead(RXB0CTRL);
  uint8_t rxb1 = mcpRead(RXB1CTRL);
  uint8_t eflg = mcpRead(EFLG);
  uint8_t intf = mcpRead(CANINTF);

  Serial.print(F("[CAN] "));
  Serial.print(tag);
  Serial.print(F(" mode="));
  Serial.print(modeToString(canstat));
  Serial.print(F(" CANSTAT=0x")); Serial.print(canstat, HEX);
  Serial.print(F(" CANCTRL=0x")); Serial.print(canctrl, HEX);
  Serial.print(F(" CNF1/2/3=0x"));
  Serial.print(cnf1, HEX); Serial.print('/');
  Serial.print(cnf2, HEX); Serial.print('/');
  Serial.print(cnf3, HEX);
  Serial.print(F(" RXB0=0x")); Serial.print(rxb0, HEX);
  Serial.print(F(" RXB1=0x")); Serial.print(rxb1, HEX);
  Serial.print(F(" EFLG=0x")); Serial.print(eflg, HEX);
  Serial.print(' ');
  if (eflg == 0) {
    Serial.print(F("(OK)"));
  } else {
    if (eflg & 0x80) Serial.print(F("RX1OVF "));
    if (eflg & 0x40) Serial.print(F("RX0OVF "));
    if (eflg & 0x20) Serial.print(F("TXBO "));
    if (eflg & 0x10) Serial.print(F("TXEP "));
    if (eflg & 0x08) Serial.print(F("RXEP "));
    if (eflg & 0x04) Serial.print(F("TXWAR "));
    if (eflg & 0x02) Serial.print(F("RXWAR "));
    if (eflg & 0x01) Serial.print(F("EWARN "));
  }
  Serial.print(F(" CANINTF=0x")); Serial.println(intf, HEX);
}

static inline uint16_t u16_lohi(uint8_t lo, uint8_t hi) {
  return ((uint16_t)hi << 8) | lo;
}

static inline int16_t s16_lohi(uint8_t lo, uint8_t hi) {
  uint16_t n = u16_lohi(lo, hi);
  return (n > 32767) ? (int16_t)(n - 65536) : (int16_t)n;
}

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

bool mcpInit500k_16MHz() {
  mcpReset();

  if (!mcpSetMode(MODE_CONFIG)) return false;

  const CanTiming &timing = kTimingPresets[MCP_TIMING_PRESET % (sizeof(kTimingPresets)/sizeof(kTimingPresets[0]))];
  mcpWrite(CNF1, timing.cnf1);
  mcpWrite(CNF2, timing.cnf2);
  mcpWrite(CNF3, timing.cnf3);

  mcpWrite(RXB0CTRL, 0x60);
  mcpWrite(RXB1CTRL, 0x60);

  mcpWrite(CANINTE, RX0IF | RX1IF);
  mcpWrite(CANINTF, 0x00);

  bool ok = mcpSetMode(MODE_NORMAL);
  Serial.print(F("[CAN] timing preset "));
  Serial.print(MCP_TIMING_PRESET);
  Serial.print(F(" ("));
  Serial.print(timing.label);
  Serial.print(F(") -> "));
  Serial.println(ok ? F("Normal") : F("FAILED"));
  return ok;
}

bool readRxBuffer(uint8_t baseAddr, Frame &f) {
  uint8_t sidh = mcpRead(baseAddr + 0);
  uint8_t sidl = mcpRead(baseAddr + 1);
  uint8_t eid8 = mcpRead(baseAddr + 2);
  uint8_t eid0 = mcpRead(baseAddr + 3);
  uint8_t dlc  = mcpRead(baseAddr + 4) & 0x0F;

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

  for (uint8_t i = 0; i < 8; i++) f.data[i] = 0;
  for (uint8_t i = 0; i < dlc && i < 8; i++) {
    f.data[i] = mcpRead(baseAddr + 5 + i);
  }

  return true;
}

void updateSnapshot(const Frame &f) {
  if (f.ext) return; // Bosch frames are standard IDs only

  if (f.id == ID_PE3_770 && f.dlc >= 7) {
    g_snap.seen_bosch = true;
    uint8_t row = f.data[0];
    g_snap.has770 = true;
    g_snap.rpm     = u16_lohi(f.data[1], f.data[2]) * 0.25f;
    g_snap.veh_kph = u16_lohi(f.data[3], f.data[4]) * 0.00781f;
    g_snap.tps_pct = f.data[5] * 0.391f;
    g_snap.ign_deg = (int8_t)f.data[6] * 1.365f;

    if (f.dlc >= 8) {
      if (row == 3) {
        g_snap.coolant_c = f.data[7] - 40.0f;
      } else if (row == 4) {
        g_snap.oil_temp_c = f.data[7] - 39.0f;
      } else if (row == 6) {
        g_snap.iat_c = f.data[7] - 40.0f;
      } else if (row == 9) {
        g_snap.gear = f.data[7];
      }
    }
  }
  else if (f.id == ID_PE3_771 && f.dlc >= 6) {
    g_snap.seen_bosch = true;
    uint8_t row = f.data[0];
    if (row == 0 && f.dlc >= 7) {
      g_snap.has771_row0 = true;
      g_snap.map_kpa = u16_lohi(f.data[5], f.data[6]) * 0.01f; // convert 0.1 mbar to kPa
    } else if (row == 1 && f.dlc >= 8) {
      g_snap.has771_row1 = true;
      g_snap.batt_v = f.data[7] * 0.111f;
    } else if (row == 3 && f.dlc >= 8) {
      g_snap.has771_row3 = true;
      g_snap.fuel_bar = f.data[5] * 0.0515f;
      g_snap.oil_bar  = f.data[7] * 0.0513f;
      g_snap.fuel_psi = g_snap.fuel_bar * BAR_TO_PSI;
      g_snap.oil_psi  = g_snap.oil_bar  * BAR_TO_PSI;
    }
  }
  else if (f.id == ID_PE3_772 && f.dlc >= 6 && f.data[0] == 0) {
    g_snap.seen_bosch = true;
    g_snap.has772 = true;
    g_snap.lambda1 = u16_lohi(f.data[2], f.data[3]) * 0.000244f;
    g_snap.lambda2 = u16_lohi(f.data[4], f.data[5]) * 0.000244f;
  }
  else if (f.id == ID_PE3_790) {
    g_snap.seen_bosch = true;
    g_snap.has790 = true;
    if (f.dlc >= 2) {
      g_snap.rpm = u16_lohi(f.data[0], f.data[1]);
    }
    if (f.dlc >= 6) {
      g_snap.ign_deg = u16_lohi(f.data[4], f.data[5]) * 0.1f;
    }
    if (f.dlc >= 8) {
      float map_psi = u16_lohi(f.data[6], f.data[7]) * 0.01f;
      g_snap.map_ext_kpa = map_psi * PSI_TO_KPA;
    }
  }
}

void printSnapshot() {
  if (!g_snap.seen_bosch) {
    Serial.println(F("===== Waiting for Bosch CAN frames @500 kbps (IDs 0x770/0x771/0x772/0x790) ====="));
    return;
  }

  Serial.println(F("===== Bosch Snapshot ====="));
  Serial.print(F("RPM: ")); Serial.print(g_snap.rpm, 1);
  Serial.print(F("  Veh: ")); Serial.print(g_snap.veh_kph, 2); Serial.println(F(" kph"));

  Serial.print(F("TPS: ")); Serial.print(g_snap.tps_pct, 1); Serial.print(F(" %"));
  Serial.print(F("  Ign: ")); Serial.print(g_snap.ign_deg, 2); Serial.println(F(" deg"));

  Serial.print(F("Coolant: "));
  if (isnan(g_snap.coolant_c)) Serial.print(F("n/a"));
  else Serial.print(g_snap.coolant_c, 1);
  Serial.print(F(" C  OilTemp: "));
  if (isnan(g_snap.oil_temp_c)) Serial.print(F("n/a"));
  else Serial.print(g_snap.oil_temp_c, 1);
  Serial.print(F(" C  IAT: "));
  if (isnan(g_snap.iat_c)) Serial.print(F("n/a"));
  else Serial.print(g_snap.iat_c, 1);
  Serial.print(F(" C  Gear: "));
  Serial.println(g_snap.gear);

  Serial.print(F("MAP: ")); Serial.print(g_snap.map_kpa, 2); Serial.print(F(" kPa"));
  Serial.print(F("  MAP_ext: ")); Serial.print(g_snap.map_ext_kpa, 2); Serial.println(F(" kPa"));

  Serial.print(F("Lambda1: ")); Serial.print(g_snap.lambda1, 3);
  Serial.print(F("  Lambda2: ")); Serial.println(g_snap.lambda2, 3);

  Serial.print(F("Battery: ")); Serial.print(g_snap.batt_v, 2); Serial.println(F(" V"));

  Serial.print(F("Fuel: ")); Serial.print(g_snap.fuel_bar, 2); Serial.print(F(" bar / "));
  Serial.print(g_snap.fuel_psi, 1); Serial.print(F(" psi"));
  Serial.print(F("  Oil: ")); Serial.print(g_snap.oil_bar, 2); Serial.print(F(" bar / "));
  Serial.print(g_snap.oil_psi, 1); Serial.println(F(" psi"));
  Serial.println();
}

uint32_t lastPrintMs = 0;
uint32_t lastStatusMs = 0;

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  Serial.begin(115200);
  delay(1000);
  Serial.println(F("\n=== BoschDecodeTest: PE3 MS4.3 monitor (500 kbps) ==="));

  pinMode(MCP_PIN_CS, OUTPUT);
  csHigh();
  pinMode(MCP_PIN_INT, INPUT_PULLUP);
  if (MCP_PIN_STANDBY >= 0) {
    pinMode(MCP_PIN_STANDBY, OUTPUT);
    digitalWrite(MCP_PIN_STANDBY, LOW);
  }
  if (MCP_PIN_RESET >= 0) {
    pinMode(MCP_PIN_RESET, OUTPUT);
    digitalWrite(MCP_PIN_RESET, HIGH);
  }

  CAN_SPI.begin();
  CAN_SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  bool ok = mcpInit500k_16MHz();
  CAN_SPI.endTransaction();

  Serial.print(F("MCP2515 init @500kbps: "));
  Serial.println(ok ? F("OK") : F("FAIL"));

  dumpCanRegisters("post-init");

#if CAN_LISTEN_ONLY
  mcpBitMod(CANCTRL, MODE_MASK, 0x60);
  bool listenOk = ((mcpRead(CANSTAT) & MODE_MASK) == 0x60);
  Serial.print(F("[CAN] Listen-only request: "));
  Serial.println(listenOk ? F("OK") : F("FAIL"));
  dumpCanRegisters("after listen-only");
#endif

  if (!ok) {
    while (true) {
      digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
      delay(200);
    }
  }
}

void loop() {
  Frame f;

  uint8_t intf = mcpRead(CANINTF);

  if (intf & RX0IF) {
    if (readRxBuffer(RXB0SIDH, f)) {
      updateSnapshot(f);
    }
    mcpBitMod(CANINTF, RX0IF, 0x00);
  }

  if (intf & RX1IF) {
    if (readRxBuffer(RXB1SIDH, f)) {
      updateSnapshot(f);
    }
    mcpBitMod(CANINTF, RX1IF, 0x00);
  }

  uint32_t now = millis();
  if (now - lastPrintMs >= 500) {
    lastPrintMs = now;
    printSnapshot();
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
  }

  if (now - lastStatusMs >= 1000) {
    lastStatusMs = now;
    dumpCanRegisters("loop");
  }
}
