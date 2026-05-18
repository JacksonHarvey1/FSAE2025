#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_NeoPixel.h>

// =====================================================================
//  SPI / CAN PINS (Feather RP2040 + CAN FeatherWing)
// =====================================================================
static constexpr uint8_t PIN_MISO = 8;
static constexpr uint8_t PIN_SCK  = 14;
static constexpr uint8_t PIN_MOSI = 15;
static constexpr uint8_t CAN_CS   = 5;

#ifndef PIN_LED
  #define PIN_LED LED_BUILTIN
#endif
static constexpr uint8_t LED_RED = PIN_LED;

// =====================================================================
//  WARNING INDICATOR LEDs (SK6812 addressable, 1 LED per pin)
//  D11 = Engine warning  (red  when CLT > CLT_WARN_F)
//  D9  = Shift indicator (blue when rpm > SHIFT_RPM)
//  D10 = Low fuel        (amber when A1 below threshold for 15s)
//  D6  = Neutral light   (green when neutral switch LOW)
// =====================================================================
#define LEDS_PER_PIN 1

static constexpr uint8_t PIN_LED_ENG_WARN = 11;
static constexpr uint8_t PIN_LED_SHIFT     = 9;
static constexpr uint8_t PIN_LED_FUEL_WARN = 10;
static constexpr uint8_t PIN_LED_NEUTRAL   = 6;

static constexpr uint8_t PIN_NEUTRAL_SWITCH = 12;  // INPUT_PULLUP: LOW = neutral

// Warning thresholds
static constexpr float    CLT_WARN_F      = 200.0f;
static constexpr float    SHIFT_RPM       = 13000.0f;
static constexpr float    FUEL_LOW_THRESH_V = 1.2f;
static constexpr uint32_t FUEL_LOW_DELAY_MS = 15000;

// LED colors
static const uint32_t COLOR_OFF   = Adafruit_NeoPixel::Color(0,   0,   0);
static const uint32_t COLOR_RED   = Adafruit_NeoPixel::Color(255, 0,   0);
static const uint32_t COLOR_GREEN = Adafruit_NeoPixel::Color(0,   255, 0);
static const uint32_t COLOR_AMBER = Adafruit_NeoPixel::Color(255, 100, 0);
static const uint32_t COLOR_BLUE  = Adafruit_NeoPixel::Color(0,   0,   255);

Adafruit_NeoPixel ledEngWarn (LEDS_PER_PIN, PIN_LED_ENG_WARN,  NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel ledShift   (LEDS_PER_PIN, PIN_LED_SHIFT,      NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel ledFuelWarn(LEDS_PER_PIN, PIN_LED_FUEL_WARN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel ledNeutral (LEDS_PER_PIN, PIN_LED_NEUTRAL,   NEO_GRB + NEO_KHZ800);

// =====================================================================
//  FUEL LEVEL SENSOR — A1 voltage threshold only (no temp calc needed)
// =====================================================================
static constexpr uint8_t PIN_FUEL_TEMP    = A1;
static constexpr uint32_t TEMP_PERIOD_MS  = 250;

// =====================================================================
//  MCP4725 DAC — Gear Indicator Output
// =====================================================================
static constexpr uint8_t DAC_ADDR = 0x60;

static const uint16_t GEAR_DAC[6] = {
  250,   // Neutral  → 0.305 V
  819,   // 1st gear → 1.00  V
  1392,  // 2nd gear → 1.70  V
  2047,  // 3rd gear → 2.50  V
  2708,  // 4th gear → 3.305 V
  3353,  // 5th gear → 4.095 V
};

// =====================================================================
//  GEAR RATIO DETECTION
// =====================================================================
struct GearBand {
  float enterMin;
  float enterMax;
  float stayMin;
  float stayMax;
};
static constexpr GearBand GEAR_BANDS[5] = {
  {228, 9999, 215, 9999},  // 1st
  {177, 228,  165, 242 },  // 2nd
  {150, 177,  140, 185 },  // 3rd
  {134, 150,  126, 158 },  // 4th
  {  0, 134,    0, 142 },  // 5th
};
static constexpr float   VEH_MOVING_KPH = 3.0f;
static constexpr float   RPM_MIN        = 500.0f;
static constexpr uint8_t GEAR_CONFIRM   = 4;

// =====================================================================
//  MCP2515 LOW-LEVEL
// =====================================================================
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
  SPI.transfer(CMD_READ); SPI.transfer(addr);
  uint8_t v = SPI.transfer(0x00);
  canCsHigh(); SPI.endTransaction();
  return v;
}
void mcpWrite(uint8_t addr, uint8_t val) {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  canCsLow();
  SPI.transfer(CMD_WRITE); SPI.transfer(addr); SPI.transfer(val);
  canCsHigh(); SPI.endTransaction();
}
void mcpBitMod(uint8_t addr, uint8_t mask, uint8_t data) {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  canCsLow();
  SPI.transfer(CMD_BITMOD); SPI.transfer(addr); SPI.transfer(mask); SPI.transfer(data);
  canCsHigh(); SPI.endTransaction();
}
void mcpReset() {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  canCsLow(); SPI.transfer(CMD_RESET); canCsHigh();
  SPI.endTransaction(); delay(10);
}
bool mcpSetMode(uint8_t mode) {
  mcpBitMod(CANCTRL, MODE_MASK, mode); delay(5);
  return ((mcpRead(CANSTAT) & MODE_MASK) == mode);
}
bool mcpInit500k_16MHz() {
  mcpReset();
  if (!mcpSetMode(MODE_CONFIG)) return false;
  mcpWrite(CNF1, 0x01); mcpWrite(CNF2, 0x90); mcpWrite(CNF3, 0x02);
  mcpWrite(RXB0CTRL, 0x60); mcpWrite(RXB1CTRL, 0x60);
  mcpWrite(CANINTE, RX0IF | RX1IF);
  mcpWrite(CANINTF, 0x00);
  return mcpSetMode(MODE_NORMAL);
}
bool readRx(uint8_t base, CanFrame &f) {
  uint8_t sidh = mcpRead(base+0), sidl = mcpRead(base+1);
  uint8_t eid8 = mcpRead(base+2), eid0 = mcpRead(base+3);
  uint8_t dlc  = mcpRead(base+4) & 0x0F;
  bool ext = (sidl & 0x08) != 0;
  uint32_t id = ext
    ? ((((uint32_t)sidh << 3) | (sidl >> 5)) << 18)
        | ((uint32_t)(sidl & 0x03) << 16) | ((uint32_t)eid8 << 8) | eid0
    : ((uint32_t)sidh << 3) | (sidl >> 5);
  f.id=id; f.ext=ext; f.rtr=(sidl&0x10)!=0; f.dlc=dlc;
  for (uint8_t i=0;i<8;i++) f.d[i]=0;
  for (uint8_t i=0;i<dlc&&i<8;i++) f.d[i]=mcpRead(base+5+i);
  return true;
}

// =====================================================================
//  GLOBALS
// =====================================================================
float    g_rpm         = 0;
float    g_veh_kph     = 0;
uint8_t  g_gear        = 0;
float    g_clt_f       = 0;
float    g_fuel_voltage = 0;
uint32_t g_fuelLowSince = 0;

uint32_t lastCanFrame  = 0;
uint32_t lastGearCalc  = 0;
uint32_t lastLedUpdate = 0;
uint32_t lastTempSample = 0;
uint32_t lastBlink     = 0;
uint32_t lastStatus    = 0;
bool     ledState      = false;

// =====================================================================
//  HELPERS
// =====================================================================
static inline uint16_t u16_lohi(uint8_t lo, uint8_t hi) {
  return ((uint16_t)hi << 8) | lo;
}

// =====================================================================
//  MCP4725 DAC
// =====================================================================
void dacWrite(uint16_t val12) {
  val12 &= 0x0FFF;
  Wire.beginTransmission(DAC_ADDR);
  Wire.write((uint8_t)(val12 >> 8));
  Wire.write((uint8_t)(val12 & 0xFF));
  uint8_t err = Wire.endTransmission();
  Serial.print(F("[DAC] val=")); Serial.print(val12);
  Serial.print(F(" V=")); Serial.print(val12 * 5.0f / 4095.0f, 3);
  Serial.print(F("  I2C err=")); Serial.println(err);
}

// =====================================================================
//  GEAR DETECTION
// =====================================================================
static uint8_t s_currentGear   = 0;
static uint8_t s_candidateGear = 0;
static uint8_t s_confirmCount  = 0;

uint8_t calculateGear(float rpm, float kph) {
  if (rpm <= 0.0f || rpm < RPM_MIN || kph < VEH_MOVING_KPH) {
    s_currentGear = 0; s_candidateGear = 0; s_confirmCount = 0;
    return 0;
  }
  float ratio = rpm / kph;
  if (s_currentGear >= 1 && s_currentGear <= 5) {
    const GearBand &b = GEAR_BANDS[s_currentGear - 1];
    if (ratio >= b.stayMin && ratio <= b.stayMax) {
      s_candidateGear = s_currentGear;
      s_confirmCount  = GEAR_CONFIRM;
      return s_currentGear;
    }
  }
  uint8_t candidate = 0;
  for (uint8_t i = 0; i < 5; i++) {
    if (ratio >= GEAR_BANDS[i].enterMin && ratio <= GEAR_BANDS[i].enterMax) {
      candidate = i + 1;
      break;
    }
  }
  if (candidate == s_candidateGear) {
    if (s_confirmCount < GEAR_CONFIRM) s_confirmCount++;
  } else {
    s_candidateGear = candidate;
    s_confirmCount  = 1;
  }
  if (s_confirmCount >= GEAR_CONFIRM) s_currentGear = s_candidateGear;
  return s_currentGear;
}

// =====================================================================
//  WARNING LED UPDATE
// =====================================================================
static void setLed(Adafruit_NeoPixel &strand, uint32_t color) {
  strand.fill(color);
  strand.show();
}

static void startupLightShow() {
  Adafruit_NeoPixel* leds[] = {&ledEngWarn, &ledShift, &ledFuelWarn, &ledNeutral};

  const uint32_t colors[] = {COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_AMBER};

  for (uint8_t c = 0; c < 4; c++) {
    for (auto *led : leds) {
      setLed(*led, colors[c]);
      delay(55);
      setLed(*led, COLOR_OFF);
    }
  }

  for (uint8_t pulse = 0; pulse < 3; pulse++) {
    for (auto *led : leds) setLed(*led, COLOR_BLUE);
    delay(90);
    for (auto *led : leds) setLed(*led, COLOR_OFF);
    delay(90);
  }
}

void updateWarningLEDs() {
  setLed(ledEngWarn, (g_clt_f > CLT_WARN_F) ? COLOR_RED : COLOR_OFF);

  uint32_t now = millis();
  if (g_fuel_voltage < FUEL_LOW_THRESH_V) {
    if (g_fuelLowSince == 0) g_fuelLowSince = now;
  } else {
    g_fuelLowSince = 0;
  }
  bool fuelLow = (g_fuelLowSince != 0) && ((now - g_fuelLowSince) >= FUEL_LOW_DELAY_MS);
  setLed(ledFuelWarn, fuelLow ? COLOR_AMBER : COLOR_OFF);

  setLed(ledNeutral, (digitalRead(PIN_NEUTRAL_SWITCH) == LOW) ? COLOR_GREEN : COLOR_OFF);
  setLed(ledShift,   (g_rpm >= SHIFT_RPM) ? COLOR_BLUE : COLOR_OFF);
}

// =====================================================================
//  CAN SNAPSHOT — only fields needed for gear/LEDs
// =====================================================================
void updateSnapshot(const CanFrame &f) {
  if (f.ext) return;
  if (f.id == 0x770 && f.dlc >= 7) {
    g_rpm     = u16_lohi(f.d[1], f.d[2]) * 0.25f;
    g_veh_kph = u16_lohi(f.d[3], f.d[4]) * 0.00781f;
    if (f.dlc >= 8 && f.d[0] == 3)
      g_clt_f = (float)f.d[7] - 40.0f;
  } else if (f.id == 0x790 && f.dlc >= 2) {
    g_rpm = u16_lohi(f.d[0], f.d[1]);
  }
}

// =====================================================================
//  SETUP
// =====================================================================
void setup() {
  pinMode(LED_RED, OUTPUT);
  digitalWrite(LED_RED, LOW);

  Serial.begin(115200);
  delay(5000);

  pinMode(PIN_NEUTRAL_SWITCH, INPUT_PULLUP);
  pinMode(PIN_FUEL_TEMP, INPUT);
  analogReadResolution(12);

  ledEngWarn.begin();  ledEngWarn.setBrightness(200);  ledEngWarn.clear();  ledEngWarn.show();
  ledFuelWarn.begin(); ledFuelWarn.setBrightness(200); ledFuelWarn.clear(); ledFuelWarn.show();
  ledNeutral.begin();  ledNeutral.setBrightness(200);  ledNeutral.clear();  ledNeutral.show();
  ledShift.begin();    ledShift.setBrightness(200);    ledShift.clear();    ledShift.show();
  startupLightShow();
  Serial.println(F("Warning LEDs: initialized + startup light show"));

  SPI.setRX(PIN_MISO);
  SPI.setTX(PIN_MOSI);
  SPI.setSCK(PIN_SCK);
  SPI.begin();

  pinMode(CAN_CS, OUTPUT); canCsHigh();

  Serial.println(F("=== Dash: Gear DAC + Warning LEDs ==="));

  bool canOk = mcpInit500k_16MHz();
  Serial.print(F("MCP2515 @500kbps: ")); Serial.println(canOk ? F("OK") : F("FAIL"));
  if (!canOk) {
    Serial.println(F("Check CAN wiring/CS/termination."));
    while (1) { digitalWrite(LED_RED, !digitalRead(LED_RED)); delay(200); }
  }

  Wire.begin();
  Wire.setClock(400000);

  dacWrite(GEAR_DAC[0]);
  Serial.println(F("MCP4725 DAC: initialized (Neutral voltage)"));

  lastCanFrame = millis();
  Serial.println(F("Dash running."));
}

// =====================================================================
//  LOOP
// =====================================================================
void loop() {
  uint32_t now = millis();

  // LED heartbeat — 1 Hz
  if (now - lastBlink >= 1000) {
    lastBlink = now;
    ledState = !ledState;
    digitalWrite(LED_RED, ledState);
  }

  // Periodic serial status — every 5s
  if (now - lastStatus >= 5000) {
    lastStatus = now;
    uint8_t eflg = mcpRead(0x2D);
    Serial.print(F("[STATUS] uptime=")); Serial.print(now / 1000);
    Serial.print(F("s gear=")); Serial.print(g_gear);
    Serial.print(F(" rpm=")); Serial.print(g_rpm, 0);
    Serial.print(F(" kph=")); Serial.print(g_veh_kph, 1);
    Serial.print(F(" clt=")); Serial.print(g_clt_f, 1); Serial.print(F("F"));
    Serial.print(F(" fuel_v=")); Serial.print(g_fuel_voltage, 3); Serial.print(F("V"));
    Serial.print(F(" EFLG=0x")); Serial.println(eflg, HEX);
  }

  // CAN timeout — zero stale values after 500ms
  if ((now - lastCanFrame) > 500UL) {
    g_rpm = 0; g_veh_kph = 0;
  }

  // Gear recalc + DAC update every 100ms
  if (now - lastGearCalc >= 100UL) {
    lastGearCalc = now;
    uint8_t newGear = calculateGear(g_rpm, g_veh_kph);
    if (newGear != g_gear) {
      g_gear = newGear;
      dacWrite(GEAR_DAC[g_gear]);
    }
  }

  // Fuel level voltage sample at 4 Hz
  if (now - lastTempSample >= TEMP_PERIOD_MS) {
    lastTempSample = now;
    int raw = analogRead(PIN_FUEL_TEMP);
    g_fuel_voltage = raw * (3.3f / 4095.0f);
  }

  // Warning LED update every 100ms
  if (now - lastLedUpdate >= 100UL) {
    lastLedUpdate = now;
    updateWarningLEDs();
  }

  // Poll CAN RX
  uint8_t intf = mcpRead(CANINTF);
  CanFrame f;

  auto handleFrame = [&](uint8_t base, uint8_t clearBit) {
    readRx(base, f);
    mcpBitMod(CANINTF, clearBit, 0x00);
    lastCanFrame = now;
    updateSnapshot(f);
  };

  if (intf & RX0IF) handleFrame(RXB0SIDH, RX0IF);
  if (intf & RX1IF) handleFrame(RXB1SIDH, RX1IF);
}
