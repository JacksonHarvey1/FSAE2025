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

// 250 kbps @ 16 MHz (your known-good CNF values)
bool mcpInit250k_16MHz() {
  mcpReset();
  if (!mcpSetMode(MODE_CONFIG)) return false;

  mcpWrite(CNF1, 0x41);
  mcpWrite(CNF2, 0xF1);
  mcpWrite(CNF3, 0x85);

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
uint32_t lastBlink = 0;
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

  // Init MCP2515
  bool canOk = mcpInit250k_16MHz();
  Serial.print("MCP2515 init: "); Serial.println(canOk ? "OK" : "FAIL");
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

  // Poll RX flags
  uint8_t intf = mcpRead(CANINTF);
  CanFrame f;

  if (intf & RX0IF) {
    readRx(RXB0SIDH, f);
    mcpBitMod(CANINTF, RX0IF, 0x00); // clear
    sendFrameBinary(f);
  }
  if (intf & RX1IF) {
    readRx(RXB1SIDH, f);
    mcpBitMod(CANINTF, RX1IF, 0x00); // clear
    sendFrameBinary(f);
  }
}
