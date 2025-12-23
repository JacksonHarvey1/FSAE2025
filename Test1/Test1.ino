#include <Arduino.h>
#include <SPI.h>

// Feather RP2040 RFM95 SPI pins (confirmed working by your loopback)
static constexpr uint8_t PIN_MISO = 8;
static constexpr uint8_t PIN_SCK  = 14;
static constexpr uint8_t PIN_MOSI = 15;

// MCP2515 CS (you confirmed CS=5 works)
static constexpr uint8_t CAN_CS = 5;

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
  bool ext;
  uint8_t dlc;
  uint8_t d[8];
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

// 250 kbps @ 16 MHz
// (Your MCP crystal marking was 16.000)
bool mcpInit250k_16MHz() {
  mcpReset();

  if (!mcpSetMode(MODE_CONFIG)) return false;

  // Bit timing: 250 kbps @ 16 MHz (matches Adafruit_MCP2515 16MHz/250k settings)
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

  f.id = id;
  f.ext = ext;
  f.dlc = dlc;

  for (uint8_t i = 0; i < dlc && i < 8; i++) {
    f.d[i] = mcpRead(base + 5 + i);
  }
  return true;
}

void printFrame(const Frame &f) {
  Serial.print("RX ");
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

// ====== PE3 AN400 decoding helpers ======
static const uint32_t ID_PE1 = 0x0CFFF048; // RPM, TPS, Fuel Open Time, Ign Angle
static const uint32_t ID_PE2 = 0x0CFFF148; // Barometer, MAP, Lambda, Pressure Type

static inline uint16_t u16_lohi(uint8_t lo, uint8_t hi) {
  return (uint16_t)hi << 8 | lo;
}

static inline int16_t s16_lohi(uint8_t lo, uint8_t hi) {
  uint16_t n = u16_lohi(lo, hi);
  if (n > 32767) return (int16_t)(n - 65536);
  return (int16_t)n;
}

void decodePEFrame(const Frame &f) {
  if (!f.ext || f.dlc < 2) return; // only handle full extended frames

  if (f.id == ID_PE1 && f.dlc >= 8) {
    // AN400: PE1
    uint16_t rpm = u16_lohi(f.d[0], f.d[1]);            // 1 rpm/bit, unsigned
    float tps    = s16_lohi(f.d[2], f.d[3]) * 0.1f;     // %
    float fot    = s16_lohi(f.d[4], f.d[5]) * 0.1f;     // ms
    float ign    = s16_lohi(f.d[6], f.d[7]) * 0.1f;     // deg

    Serial.print("  PE1: RPM="); Serial.print(rpm);
    Serial.print(" TPS="); Serial.print(tps, 1); Serial.print(" %");
    Serial.print(" FOT="); Serial.print(fot, 1); Serial.print(" ms");
    Serial.print(" IGN="); Serial.print(ign, 1); Serial.println(" deg");
  }
  else if (f.id == ID_PE2 && f.dlc >= 8) {
    // AN400: PE2
    float baro = s16_lohi(f.d[0], f.d[1]) * 0.01f;      // psi or kPa
    float map  = s16_lohi(f.d[2], f.d[3]) * 0.01f;      // psi or kPa
    float lam  = s16_lohi(f.d[4], f.d[5]) * 0.01f;      // lambda
    bool kpa   = (f.d[6] & 0x01) != 0;                  // pressure type bit

    Serial.print("  PE2: BARO="); Serial.print(baro, 2);
    Serial.print(kpa ? " kPa" : " psi");
    Serial.print(" MAP="); Serial.print(map, 2);
    Serial.print(kpa ? " kPa" : " psi");
    Serial.print(" LAMBDA="); Serial.println(lam, 2);
  }
}

uint32_t lastBlink = 0;
bool led = false;
uint32_t lastRxMs = 0;
uint32_t lastStat = 0;

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  Serial.begin(115200);
  delay(800);

  Serial.println("=== MCP2515 RAW SNIFFER ===");
  Serial.println("Slow blink = running; fast blink = pulled a frame");

  pinMode(CAN_CS, OUTPUT);
  csHigh();

  CAN_SPI.begin();

  CAN_SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  bool ok = mcpInit250k_16MHz();
  CAN_SPI.endTransaction();

  Serial.print("[MCP] init 250k/16MHz => ");
  Serial.println(ok ? "OK" : "FAIL");
  Serial.print("[MCP] CANSTAT=0x");
  Serial.println(mcpRead(CANSTAT), HEX);
}

void loop() {
  uint32_t now = millis();

  // Blink always (proof running)
  uint32_t period = (now - lastRxMs < 300) ? 120 : 700;
  if (now - lastBlink > period) {
    lastBlink = now;
    led = !led;
    digitalWrite(LED_BUILTIN, led);
  }

  // Print status every second so we see what's happening even without RX frames
  if (now - lastStat > 1000) {
    lastStat = now;
    uint8_t intf = mcpRead(CANINTF);
    uint8_t eflg = mcpRead(EFLG);
    uint8_t tec  = mcpRead(TEC);
    uint8_t rec  = mcpRead(REC);

    Serial.print("STAT: CANINTF=0x"); Serial.print(intf, HEX);
    Serial.print(" EFLG=0x"); Serial.print(eflg, HEX);
    Serial.print(" TEC="); Serial.print(tec);
    Serial.print(" REC="); Serial.println(rec);

    // Clear overflow flags if present (RX0OVR=0x20, RX1OVR=0x40)
    if (eflg & 0x60) {
      mcpBitMod(EFLG, 0x60, 0x00);
      Serial.println("  cleared RX overflow flags");
    }
  }

  // Poll for RX flags and actually read buffers
  uint8_t intf = mcpRead(CANINTF);
  Frame f{};

  if (intf & RX0IF) {
    readRx(RXB0SIDH, f);
    mcpBitMod(CANINTF, RX0IF, 0x00); // clear RX0IF
    lastRxMs = now;
    printFrame(f);
    decodePEFrame(f);
  }

  if (intf & RX1IF) {
    readRx(RXB1SIDH, f);
    mcpBitMod(CANINTF, RX1IF, 0x00); // clear RX1IF
    lastRxMs = now;
    printFrame(f);
    decodePEFrame(f);
  }
}
