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

// Send a machine‑readable line to the Pi.
void sendFrameToPi(const Frame &f) {
  Serial.print("CAN,");
  Serial.print(millis());
  Serial.print(",");
  Serial.print(f.id, HEX);        // hex, no 0x prefix
  Serial.print(",");
  Serial.print(f.ext ? 1 : 0);
  Serial.print(",");
  Serial.print(f.dlc);
  for (uint8_t i = 0; i < 8; i++) {
    Serial.print(",");
    Serial.print(f.d[i]);         // decimal byte 0–255
  }
  Serial.println();
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

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  Serial.begin(115200);
  delay(800);

  Serial.println("# RP2040 CAN→USB bridge starting");
  Serial.println("# Line format: CAN,ts_ms,id_hex,ext,dlc,b0..b7");

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
    sendFrameToPi(f);
  }

  if (intf & RX1IF) {
    readRx(RXB1SIDH, f);
    mcpBitMod(CANINTF, RX1IF, 0x00); // clear RX1IF
    printFrameDebug(f);
    sendFrameToPi(f);
  }
}

