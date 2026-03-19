#include <Arduino.h>
#include <SPI.h>
#include <RH_RF95.h>

// --------------------- USER SETTINGS ---------------------
static constexpr float  LORA_FREQ_MHZ = 915.0;  // change if needed (915/868/433)
static constexpr uint8_t LORA_TX_POWER_DBM = 20;

// Feather RP2040 SPI pins (forced)
static constexpr uint8_t PIN_MISO = 8;
static constexpr uint8_t PIN_SCK  = 14;
static constexpr uint8_t PIN_MOSI = 15;

// CAN (MCP2515) chip select (from your prior code)
static constexpr uint8_t CAN_CS = 5;

// RFM95 LoRa pins (from your working receiver test)
static constexpr uint8_t RFM95_CS  = 16;
static constexpr uint8_t RFM95_INT = 21;
static constexpr uint8_t RFM95_RST = 17;

// LED
#ifndef PIN_LED
  #define PIN_LED LED_BUILTIN
#endif
static constexpr uint8_t LED_RED = PIN_LED;

// --------------------- RadioHead LoRa ---------------------
RH_RF95 rf95(RFM95_CS, RFM95_INT);

// --------------------- MCP2515 low-level ---------------------
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

#define RXB0CTRL 0x60
#define RXB1CTRL 0x70

#define RXB0SIDH 0x61
#define RXB1SIDH 0x71

#define RX0IF 0x01
#define RX1IF 0x02

#define MODE_MASK   0xE0
#define MODE_CONFIG 0x80
#define MODE_NORMAL 0x00

struct CanFrame {
  uint32_t id = 0;
  bool ext = false;
  bool rtr = false;
  uint8_t dlc = 0;
  uint8_t d[8] = {0};
};

static inline void canCsLow()  { digitalWrite(CAN_CS, LOW); }
static inline void canCsHigh() { digitalWrite(CAN_CS, HIGH); }

uint8_t mcpRead(uint8_t addr) {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  canCsLow();
  SPI.transfer(CMD_READ);
  SPI.transfer(addr);
  uint8_t v = SPI.transfer(0x00);
  canCsHigh();
  SPI.endTransaction();
  return v;
}

void mcpWrite(uint8_t addr, uint8_t val) {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  canCsLow();
  SPI.transfer(CMD_WRITE);
  SPI.transfer(addr);
  SPI.transfer(val);
  canCsHigh();
  SPI.endTransaction();
}

void mcpBitMod(uint8_t addr, uint8_t mask, uint8_t data) {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  canCsLow();
  SPI.transfer(CMD_BITMOD);
  SPI.transfer(addr);
  SPI.transfer(mask);
  SPI.transfer(data);
  canCsHigh();
  SPI.endTransaction();
}

void mcpReset() {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  canCsLow();
  SPI.transfer(CMD_RESET);
  canCsHigh();
  SPI.endTransaction();
  delay(10);
}

bool mcpSetMode(uint8_t mode) {
  mcpBitMod(CANCTRL, MODE_MASK, mode);
  delay(5);
  return ((mcpRead(CANSTAT) & MODE_MASK) == mode);
}

// 500 kbps @ 16 MHz — matches Bosch MS4.3 (PE3 8400) ECU
// BRP1 preset: CNF1=0x01, CNF2=0x90, CNF3=0x02 (sample ~70%)
bool mcpInit500k_16MHz() {
  mcpReset();
  if (!mcpSetMode(MODE_CONFIG)) return false;

  mcpWrite(CNF1, 0x01);
  mcpWrite(CNF2, 0x90);
  mcpWrite(CNF3, 0x02);

  // Receive any
  mcpWrite(RXB0CTRL, 0x60);
  mcpWrite(RXB1CTRL, 0x60);

  // Enable RX interrupts
  mcpWrite(CANINTE, RX0IF | RX1IF);

  // Clear flags
  mcpWrite(CANINTF, 0x00);

  return mcpSetMode(MODE_NORMAL);
}

bool readRx(uint8_t base, CanFrame &f) {
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
  f.rtr = (sidl & 0x10) != 0; // not perfect for all cases, but fine for sniff/test
  f.dlc = dlc;

  for (uint8_t i = 0; i < 8; i++) f.d[i] = 0;
  for (uint8_t i = 0; i < dlc && i < 8; i++) {
    f.d[i] = mcpRead(base + 5 + i);
  }
  return true;
}

// --------------------- Bosch decode (serial debug only) ---------------------
static constexpr float BAR_TO_PSI = 14.503774f;
static constexpr float PSI_TO_KPA = 6.894757f;

static inline uint16_t u16_lohi(uint8_t lo, uint8_t hi) { return ((uint16_t)hi << 8) | lo; }

void printDecoded(const CanFrame &f) {
  if (f.ext) return;
  if (f.id == 0x770 && f.dlc >= 7) {
    Serial.print(F("  -> RPM=")); Serial.print(u16_lohi(f.d[1], f.d[2]) * 0.25f, 0);
    Serial.print(F(" TPS=")); Serial.print(f.d[5] * 0.391f, 1); Serial.print('%');
    Serial.print(F(" IGN=")); Serial.print((int8_t)f.d[6] * 1.365f, 1); Serial.print(F("deg"));
    if (f.dlc >= 8) {
      uint8_t row = f.d[0];
      if (row == 3) { Serial.print(F(" Coolant=")); Serial.print(f.d[7] - 40); Serial.print('C'); }
      if (row == 4) { Serial.print(F(" OilT="));    Serial.print(f.d[7] - 39); Serial.print('C'); }
      if (row == 6) { Serial.print(F(" IAT="));     Serial.print(f.d[7] - 40); Serial.print('C'); }
      if (row == 9) { Serial.print(F(" Gear="));    Serial.print(f.d[7]); }
    }
    Serial.println();
  } else if (f.id == 0x771 && f.dlc >= 6) {
    uint8_t row = f.d[0];
    if (row == 0 && f.dlc >= 7) { Serial.print(F("  -> MAP=")); Serial.print(u16_lohi(f.d[5], f.d[6]) * 0.01f, 1); Serial.println(F(" kPa")); }
    if (row == 1 && f.dlc >= 8) { Serial.print(F("  -> Batt=")); Serial.print(f.d[7] * 0.111f, 2); Serial.println('V'); }
    if (row == 3 && f.dlc >= 8) {
      Serial.print(F("  -> Fuel=")); Serial.print(f.d[5] * 0.0515f * BAR_TO_PSI, 1); Serial.print(F("psi"));
      Serial.print(F(" Oil="));  Serial.print(f.d[7] * 0.0513f * BAR_TO_PSI, 1); Serial.println(F("psi"));
    }
  } else if (f.id == 0x772 && f.dlc >= 3) {
    uint8_t row = f.d[0];
    if (row == 0 && f.dlc >= 6) {
      Serial.print(F("  -> L1=")); Serial.print(u16_lohi(f.d[2], f.d[3]) * 0.000244f, 3);
      Serial.print(F(" L2=")); Serial.println(u16_lohi(f.d[4], f.d[5]) * 0.000244f, 3);
    } else if (row == 3) {
      Serial.print(F("  -> InjTime=")); Serial.print(f.d[2] * 0.8192f, 2); Serial.println(F(" ms"));
    } else {
      Serial.print(F("  -> 0x772 row=")); Serial.print(row); Serial.println(F(" (no signals defined in DBC)"));
    }
  } else if (f.id == 0x790) {
    if (f.dlc >= 2) { Serial.print(F("  -> RPM=")); Serial.print(u16_lohi(f.d[0], f.d[1])); }
    if (f.dlc >= 6) { Serial.print(F(" IGN=")); Serial.print(u16_lohi(f.d[4], f.d[5]) * 0.1f, 1); Serial.print(F("deg")); }
    if (f.dlc >= 8) { Serial.print(F(" MAP=")); Serial.print(u16_lohi(f.d[6], f.d[7]) * 0.01f * PSI_TO_KPA, 1); Serial.print(F("kPa")); }
    Serial.println();
  }
}

// --------------------- Binary LoRa send ---------------------
static uint32_t g_seq = 0;

static inline void put_u32_le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

bool sendFrameBinary(const CanFrame &f) {
  uint8_t pkt[24] = {0};
  pkt[0] = 0xA5;
  pkt[1] = 0x01;

  uint8_t flags = 0;
  if (f.ext) flags |= 0x01;
  if (f.rtr) flags |= 0x02;
  pkt[2] = flags;
  pkt[3] = (uint8_t)min<uint8_t>(f.dlc, 8);

  put_u32_le(&pkt[4], f.id);
  put_u32_le(&pkt[8], (uint32_t)millis());

  for (uint8_t i = 0; i < 8; i++) pkt[12 + i] = f.d[i];

  g_seq++;
  put_u32_le(&pkt[20], g_seq);

  // IMPORTANT: keep CAN_CS high while using radio (shared SPI bus)
  canCsHigh();

  rf95.send(pkt, sizeof(pkt));
  rf95.waitPacketSent();
  return true;
}

// --------------------- Setup/Loop ---------------------
uint32_t lastBlink  = 0;
uint32_t lastStatus = 0;
bool ledState = false;

void setup() {
  pinMode(LED_RED, OUTPUT);
  digitalWrite(LED_RED, LOW);

  Serial.begin(115200);
  delay(600);

  // Force SPI pins (your known-good fix)
  SPI.setRX(PIN_MISO);
  SPI.setTX(PIN_MOSI);
  SPI.setSCK(PIN_SCK);
  SPI.begin();

  // Make sure CAN is deselected by default
  pinMode(CAN_CS, OUTPUT);
  canCsHigh();

  // Radio reset
  pinMode(RFM95_RST, OUTPUT);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);
  digitalWrite(RFM95_RST, LOW);
  delay(10);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);

  Serial.println("=== CAN->LoRa TX (binary) ===");

  // Init radio (CAN_CS must be HIGH or it can kill SPI)
  if (!rf95.init()) {
    Serial.println("ERROR: rf95.init() failed");
    while (1) { digitalWrite(LED_RED, !digitalRead(LED_RED)); delay(100); }
  }
  if (!rf95.setFrequency(LORA_FREQ_MHZ)) {
    Serial.println("ERROR: setFrequency failed");
    while (1) { digitalWrite(LED_RED, !digitalRead(LED_RED)); delay(100); }
  }
  rf95.setTxPower(LORA_TX_POWER_DBM, false);
  Serial.print("LoRa OK @ "); Serial.print(LORA_FREQ_MHZ); Serial.println(" MHz");

  // Init MCP2515 @ 500 kbps (Bosch MS4.3 / PE3 8400)
  bool canOk = mcpInit500k_16MHz();
  Serial.print("MCP2515 init @500kbps: "); Serial.println(canOk ? "OK" : "FAIL");
  if (!canOk) {
    Serial.println("Check CAN wiring/CS/termination/16MHz crystal.");
    while (1) { digitalWrite(LED_RED, !digitalRead(LED_RED)); delay(200); }
  }

  Serial.println("TX running. Sending any received CAN frames as 24B packets.");
}

void loop() {
  // Red LED blink every 1 second (TX requirement)
  uint32_t now = millis();
  if (now - lastBlink >= 1000) {
    lastBlink = now;
    ledState = !ledState;
    digitalWrite(LED_RED, ledState);
  }

  // Periodic status: CANSTAT, EFLG, CANINTF, seq count — every 2 seconds
  if (now - lastStatus >= 2000) {
    lastStatus = now;
    uint8_t canstat = mcpRead(CANSTAT);
    uint8_t eflg    = mcpRead(0x2D);  // EFLG
    uint8_t intf_s  = mcpRead(CANINTF);
    Serial.print(F("[TX] uptime=")); Serial.print(now / 1000);
    Serial.print(F("s  seq=")); Serial.print(g_seq);
    Serial.print(F("  CANSTAT=0x")); Serial.print(canstat, HEX);
    Serial.print(F("  EFLG=0x")); Serial.print(eflg, HEX);
    if (eflg & 0x20) Serial.print(F(" TXBO"));
    if (eflg & 0x10) Serial.print(F(" TXEP"));
    if (eflg & 0x08) Serial.print(F(" RXEP"));
    if (eflg & 0x40) Serial.print(F(" RX0OVF"));
    if (eflg & 0x80) Serial.print(F(" RX1OVF"));
    if (eflg == 0)   Serial.print(F(" OK"));
    Serial.print(F("  INTF=0x")); Serial.println(intf_s, HEX);
  }

  // Poll RX flags
  uint8_t intf = mcpRead(CANINTF);
  CanFrame f;

  if (intf & RX0IF) {
    readRx(RXB0SIDH, f);
    mcpBitMod(CANINTF, RX0IF, 0x00); // clear
    Serial.print(F("[TX] RX0 ID=0x")); Serial.print(f.id, HEX);
    Serial.print(F(" DLC=")); Serial.print(f.dlc);
    Serial.print(F(" DATA="));
    for (uint8_t i = 0; i < f.dlc && i < 8; i++) {
      if (f.d[i] < 16) Serial.print('0');
      Serial.print(f.d[i], HEX); Serial.print(' ');
    }
    Serial.println();
    printDecoded(f);
    sendFrameBinary(f);
  }
  if (intf & RX1IF) {
    readRx(RXB1SIDH, f);
    mcpBitMod(CANINTF, RX1IF, 0x00); // clear
    Serial.print(F("[TX] RX1 ID=0x")); Serial.print(f.id, HEX);
    Serial.print(F(" DLC=")); Serial.print(f.dlc);
    Serial.print(F(" DATA="));
    for (uint8_t i = 0; i < f.dlc && i < 8; i++) {
      if (f.d[i] < 16) Serial.print('0');
      Serial.print(f.d[i], HEX); Serial.print(' ');
    }
    Serial.println();
    printDecoded(f);
    sendFrameBinary(f);
  }
}
