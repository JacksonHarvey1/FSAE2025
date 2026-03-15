#include <Arduino.h>
#include <SPI.h>

static const uint8_t CAN_CS_PIN  = 5;
static const uint8_t CAN_INT_PIN = 6;

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
#define TEC       0x1C
#define REC       0x1D

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

struct Frame {
  uint32_t id;
  bool ext;
  uint8_t dlc;
  uint8_t data[8];
};

static inline void csLow()  { digitalWrite(CAN_CS_PIN, LOW); }
static inline void csHigh() { digitalWrite(CAN_CS_PIN, HIGH); }

static inline uint16_t u16_lohi(uint8_t lo, uint8_t hi) {
  return ((uint16_t)hi << 8) | lo;
}

static inline int16_t s16_lohi(uint8_t lo, uint8_t hi) {
  uint16_t n = u16_lohi(lo, hi);
  return (n > 32767) ? (int16_t)(n - 65536) : (int16_t)n;
}

static constexpr float BAR_TO_PSI = 14.503774f;
static constexpr float PSI_TO_KPA = 6.894757f;

uint8_t mcpRead(uint8_t addr) {
  csLow();
  SPI.transfer(CMD_READ);
  SPI.transfer(addr);
  uint8_t v = SPI.transfer(0x00);
  csHigh();
  return v;
}

void mcpWrite(uint8_t addr, uint8_t val) {
  csLow();
  SPI.transfer(CMD_WRITE);
  SPI.transfer(addr);
  SPI.transfer(val);
  csHigh();
}

void mcpBitMod(uint8_t addr, uint8_t mask, uint8_t data) {
  csLow();
  SPI.transfer(CMD_BITMOD);
  SPI.transfer(addr);
  SPI.transfer(mask);
  SPI.transfer(data);
  csHigh();
}

void mcpReset() {
  csLow();
  SPI.transfer(CMD_RESET);
  csHigh();
  delay(10);
}

bool mcpSetMode(uint8_t mode) {
  mcpBitMod(CANCTRL, MODE_MASK, mode);
  delay(5);
  return ((mcpRead(CANSTAT) & MODE_MASK) == mode);
}

// Keep your known-good timing
bool mcpInit500k_16MHz() {
  mcpReset();

  if (!mcpSetMode(MODE_CONFIG)) return false;

  mcpWrite(CNF1, 0x01);
  mcpWrite(CNF2, 0x90);
  mcpWrite(CNF3, 0x02);

  mcpWrite(RXB0CTRL, 0x60);
  mcpWrite(RXB1CTRL, 0x60);

  mcpWrite(CANINTE, RX0IF | RX1IF);
  mcpWrite(CANINTF, 0x00);

  return mcpSetMode(MODE_NORMAL);
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

void printDecoded(const Frame &f) {
  if (f.id == ID_PE3_770 && f.dlc >= 7) {
    uint8_t row = f.data[0];
    float rpm = u16_lohi(f.data[1], f.data[2]) * 0.25f;
    float veh_kph = u16_lohi(f.data[3], f.data[4]) * 0.00781f;
    float tps_pct = f.data[5] * 0.391f;
    float ign_deg = (int8_t)f.data[6] * 1.365f;

    Serial.print("0x770 row=");
    Serial.print(row);
    Serial.print("  RPM=");
    Serial.print(rpm, 1);
    Serial.print("  Veh=");
    Serial.print(veh_kph, 2);
    Serial.print(" kph  TPS=");
    Serial.print(tps_pct, 1);
    Serial.print("%  Ign=");
    Serial.print(ign_deg, 1);
    Serial.print(" deg");

    if (f.dlc >= 8) {
      Serial.print("  extra=");
      if (row == 3) {
        Serial.print("Coolant ");
        Serial.print(f.data[7] - 40.0f, 1);
        Serial.print(" C");
      } else if (row == 4) {
        Serial.print("OilTemp ");
        Serial.print(f.data[7] - 39.0f, 1);
        Serial.print(" C");
      } else if (row == 6) {
        Serial.print("IAT ");
        Serial.print(f.data[7] - 40.0f, 1);
        Serial.print(" C");
      } else if (row == 9) {
        Serial.print("Gear ");
        Serial.print(f.data[7]);
      } else {
        Serial.print("byte7=0x");
        if (f.data[7] < 16) Serial.print('0');
        Serial.print(f.data[7], HEX);
      }
    }
    Serial.println();
  }
  else if (f.id == ID_PE3_771 && f.dlc >= 6) {
    uint8_t row = f.data[0];
    Serial.print("0x771 row=");
    Serial.print(row);
    if (row == 0 && f.dlc >= 7) {
      float map_kpa = u16_lohi(f.data[5], f.data[6]) * 0.01f;
      Serial.print("  MAP=");
      Serial.print(map_kpa, 2);
      Serial.print(" kPa");
    } else if (row == 1 && f.dlc >= 8) {
      float batt_v = f.data[7] * 0.111f;
      Serial.print("  Batt=");
      Serial.print(batt_v, 2);
      Serial.print(" V");
    } else if (row == 3 && f.dlc >= 8) {
      float fuel_bar = f.data[5] * 0.0515f;
      float oil_bar  = f.data[7] * 0.0513f;
      Serial.print("  Fuel=");
      Serial.print(fuel_bar, 2);
      Serial.print(" bar (");
      Serial.print(fuel_bar * BAR_TO_PSI, 1);
      Serial.print(" psi)");
      Serial.print("  Oil=");
      Serial.print(oil_bar, 2);
      Serial.print(" bar (");
      Serial.print(oil_bar * BAR_TO_PSI, 1);
      Serial.print(" psi)");
    } else {
      Serial.print("  payload bytes: ");
      for (uint8_t i = 0; i < f.dlc; ++i) {
        if (f.data[i] < 16) Serial.print('0');
        Serial.print(f.data[i], HEX);
        if (i + 1 < f.dlc) Serial.print(' ');
      }
    }
    Serial.println();
  }
  else if (f.id == ID_PE3_772 && f.dlc >= 6 && f.data[0] == 0) {
    float lambda1 = u16_lohi(f.data[2], f.data[3]) * 0.000244f;
    float lambda2 = u16_lohi(f.data[4], f.data[5]) * 0.000244f;
    Serial.print("0x772  Lambda1=");
    Serial.print(lambda1, 3);
    Serial.print("  Lambda2=");
    Serial.println(lambda2, 3);
  }
  else if (f.id == ID_PE3_790) {
    Serial.print("0x790  ExtRPM=");
    if (f.dlc >= 2) {
      Serial.print(u16_lohi(f.data[0], f.data[1]));
    } else {
      Serial.print("(dlc<2)");
    }
    if (f.dlc >= 6) {
      Serial.print("  ExtIgn=");
      Serial.print(u16_lohi(f.data[4], f.data[5]) * 0.1f, 1);
    }
    if (f.dlc >= 8) {
      float map_ext = u16_lohi(f.data[6], f.data[7]) * 0.01f * PSI_TO_KPA;
      Serial.print("  ExtMAP=");
      Serial.print(map_ext, 2);
      Serial.print(" kPa");
    }
    Serial.println();
  }
}

uint32_t lastStat = 0;

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  Serial.begin(115200);
  delay(1000);

  Serial.println("Starting PE3 Bosch CAN monitor...");

  pinMode(CAN_CS_PIN, OUTPUT);
  csHigh();
  pinMode(CAN_INT_PIN, INPUT_PULLUP);

  SPI.begin();
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  bool ok = mcpInit500k_16MHz();
  SPI.endTransaction();

  Serial.print("MCP2515 init: ");
  Serial.println(ok ? "OK" : "FAIL");

  if (!ok) {
    while (1) {
      digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
      delay(200);
    }
  }
}

void loop() {
  uint32_t now = millis();
  Frame f;

  uint8_t intf = mcpRead(CANINTF);

  if (intf & RX0IF) {
    if (readRxBuffer(RXB0SIDH, f)) {
      printDecoded(f);
    }
    mcpBitMod(CANINTF, RX0IF, 0x00);
  }

  if (intf & RX1IF) {
    if (readRxBuffer(RXB1SIDH, f)) {
      printDecoded(f);
    }
    mcpBitMod(CANINTF, RX1IF, 0x00);
  }

  if (mcpRead(EFLG) & 0xC0) {
    mcpBitMod(EFLG, 0xC0, 0x00);
  }

  if (now - lastStat >= 1000) {
    lastStat = now;
    Serial.print("# TEC=");
    Serial.print(mcpRead(TEC));
    Serial.print(" REC=");
    Serial.println(mcpRead(REC));
  }

  delay(1);
}
