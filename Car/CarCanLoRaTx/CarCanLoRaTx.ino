#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <SdFat.h>
#include <RH_RF95.h>
#include <Adafruit_LSM6DSOX.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_NeoPixel.h>

// =====================================================================
//  USER SETTINGS
// =====================================================================
static constexpr float   LORA_FREQ_MHZ     = 915.0;
static constexpr uint8_t LORA_TX_POWER_DBM = 17;  // reduced from 20 to limit TX current spike (~120mA→~65mA) which was drooping 3.3V rail and resetting IMU

// SPI bus pins (Feather RP2040 RFM95)
static constexpr uint8_t PIN_MISO = 8;
static constexpr uint8_t PIN_SCK  = 14;
static constexpr uint8_t PIN_MOSI = 15;

// SPI chip selects
static constexpr uint8_t CAN_CS   = 5;   // GP5 — Adafruit CAN FeatherWing CS (confirmed from DynoRP2040_MCP2515Wing)
static constexpr uint8_t RFM95_CS = 16;
static constexpr uint8_t SD_CS    = 25;  // D25 (GP25) on Feather RP2040

// RFM95 control pins
static constexpr uint8_t RFM95_INT = 21;
static constexpr uint8_t RFM95_RST = 17;

#ifndef PIN_LED
  #define PIN_LED LED_BUILTIN
#endif
static constexpr uint8_t LED_RED = PIN_LED;

// =====================================================================
//  WARNING INDICATOR LEDs (SK6812 addressable, 1 LED per pin)
// =====================================================================
//  D7  = Engine warning        (red when CLT > CLT_WARN_C)  — moved from D5 (D5 = CAN CS)
//  D6  = Oil pressure warning  (red when oil_psi < OIL_WARN_PSI while engine running)
//  D9  = Shift indicator       (blue when rpm > SHIFT_RPM)
//  D10 = Low fuel warning      (amber when fuel thermistor > FUEL_LOW_THRESH_C)
//  D11 = Neutral light         (green when gear == 0)
//  D5  = CAN FeatherWing chip select — not available as LED pin
//
// Requires Adafruit NeoPixel library (Tools > Manage Libraries > "Adafruit NeoPixel")

#define LEDS_PER_PIN 1

static constexpr uint8_t PIN_LED_ENG_WARN  = 7;   // moved from 5 — pin 5 is CAN FeatherWing CS
static constexpr uint8_t PIN_LED_OIL_WARN  = 6;
static constexpr uint8_t PIN_LED_SHIFT      = 9;
static constexpr uint8_t PIN_LED_FUEL_WARN = 10;
static constexpr uint8_t PIN_LED_NEUTRAL   = 11;

// Neutral switch input — wire neutral position sensor/switch to this pin and GND
// INPUT_PULLUP: LOW = neutral, HIGH = in gear
static constexpr uint8_t PIN_NEUTRAL_SWITCH = 4;

// Warning thresholds — tune as needed
static constexpr float    CLT_WARN_F      = 200.0f;  // coolant temp warning (°F)
static constexpr float    OIL_WARN_PSI    =  30.0f;  // oil pressure warning (psi)
static constexpr float    SHIFT_RPM       = 13000.0f; // shift indicator RPM

// LED colors
static const uint32_t COLOR_OFF   = Adafruit_NeoPixel::Color(0,   0,   0);
static const uint32_t COLOR_RED   = Adafruit_NeoPixel::Color(255, 0,   0);
static const uint32_t COLOR_GREEN = Adafruit_NeoPixel::Color(0,   255, 0);
static const uint32_t COLOR_AMBER = Adafruit_NeoPixel::Color(255, 100, 0);
static const uint32_t COLOR_BLUE  = Adafruit_NeoPixel::Color(0,   0,   255);

Adafruit_NeoPixel ledEngWarn (LEDS_PER_PIN, PIN_LED_ENG_WARN,  NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel ledOilWarn (LEDS_PER_PIN, PIN_LED_OIL_WARN,  NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel ledShift   (LEDS_PER_PIN, PIN_LED_SHIFT,      NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel ledFuelWarn(LEDS_PER_PIN, PIN_LED_FUEL_WARN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel ledNeutral (LEDS_PER_PIN, PIN_LED_NEUTRAL,   NEO_GRB + NEO_KHZ800);

// =====================================================================
//  STEERING ANGLE SENSOR (A0 / GP26)
// =====================================================================
// Potentiometer: 0–5V, scaled to 0–3.3V via voltage divider before ADC.
// Formula uses original 0–5V signal: θ = 75 × (V_orig − 3.35) / 1.045
// ADC back-calculates: V_orig = V_adc × (5.0 / 3.3)
// RP2040 ADC: 12-bit (0–4095), 3.3V reference.
static constexpr uint8_t  PIN_STEER       = A0;  // GP26
static constexpr uint32_t STEER_PERIOD_MS = 20;  // 50 Hz

// =====================================================================
//  FUEL LEVEL SENSOR — PANW 103395-395 NTC thermistor (A1 / GP27)
// =====================================================================
// Thermal dispersion method: current through thermistor causes self-heating.
//   Submerged in fuel → fuel conducts heat away → thermistor reads COOLER
//   In air (fuel below sensor) → no cooling → thermistor reads HOTTER
// Wiring: 3.3V → 10kΩ pull-up → A1 → thermistor → GND
// CALIBRATION: set FUEL_LOW_THRESH_C to midpoint between your "in fuel" and
//              "in air" steady-state readings. Typically ~10–20°C above ambient.
static constexpr uint8_t  PIN_FUEL_TEMP      = A1;       // GP27
static constexpr float    FUEL_R_PULLUP      = 10000.0f; // 10kΩ pull-up resistor
static constexpr float    FUEL_R25           = 10000.0f; // thermistor R at 25°C
static constexpr float    FUEL_BETA          = 3950.0f;  // Beta constant (K)
static constexpr float    FUEL_T0_K          = 298.15f;  // 25°C in Kelvin
static constexpr float    FUEL_LOW_THRESH_V  = 1.64f;    // A1 voltage below this = fuel low
static constexpr uint32_t FUEL_LOW_DELAY_MS = 15000;    // must stay below threshold for this long before warning

// =====================================================================
//  OIL TEMPERATURE 2 — eBay universal 1/8 NPT sender (A3 / GP29)
// =====================================================================
// Oil temp 1 comes from the PE3 ECU via CAN (g_oil_temp1_c, decoded from 0x770 row 4).
// Oil temp 2 is this external sender — second measurement point on the engine.
// Variable resistance sender, range 0–150°C. Single-wire — signal on wire, ground
// through threaded body into engine block. Engine block MUST share GND with RP2040.
// Wiring: 3.3V → 1kΩ pull-up → A3 → signal wire → [sender body] → engine block → GND
// CALIBRATION NEEDED: measure sender resistance at known temperatures and
// fill in OIL2_CAL_TEMP[] / OIL2_CAL_OHMS[] below. Current values are estimates.
static constexpr uint8_t  PIN_OIL_TEMP2      = A3;     // GP29
static constexpr float    OIL2_R_PULLUP      = 1000.0f; // 1kΩ pull-up resistor

// Calibration table — (resistance Ω → temperature °C). Add more points for accuracy.
// TO CALIBRATE: measure V_adc at known temps, compute R = R_pullup * V_adc / (3.3 - V_adc)
static constexpr int      OIL2_CAL_POINTS = 5;
static constexpr float    OIL2_CAL_OHMS[OIL2_CAL_POINTS] = { 180.0f, 130.0f, 90.0f, 60.0f, 35.0f };
static constexpr float    OIL2_CAL_TEMP[OIL2_CAL_POINTS] = {  30.0f,  60.0f, 90.0f, 120.0f, 150.0f };

static constexpr uint32_t TEMP_PERIOD_MS = 250; // sample both temp sensors at 4 Hz

// =====================================================================
//  MCP4725 DAC — Gear Indicator Output
// =====================================================================
static constexpr uint8_t DAC_ADDR = 0x60;

// PE3 ECU Analog #5 gear voltage midpoints (from Setup Gear screen):
//   Neutral: 0.00–0.61 V  → midpoint 0.305 V
//   1st:     0.80–1.20 V  → midpoint 1.00  V
//   2nd:     1.39–2.00 V  → midpoint 1.695 V
//   3rd:     2.20–2.80 V  → midpoint 2.50  V
//   4th:     3.00–3.61 V  → midpoint 3.305 V
//   5th:     3.80–4.39 V  → midpoint 4.095 V
// DAC value = (V / 5.0) * 4095  (12-bit, VCC=5V)
// Index 0 = Neutral, 1-5 = Gears 1-5
static const uint16_t GEAR_DAC[6] = {
  250,   // Neutral  → 0.305 V
  819,   // 1st gear → 1.00  V
  1392,  // 2nd gear → 1.70  V
  2047,  // 3rd gear → 2.50  V
  2708,  // 4th gear → 3.305 V
  3353,  // 5th gear → 4.095 V
};

// =====================================================================
//  GEAR RATIO DETECTION — band + hysteresis
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
static constexpr float VEH_MOVING_KPH  = 3.0f;
static constexpr float RPM_MIN         = 500.0f;
static constexpr uint8_t GEAR_CONFIRM  = 4;

// =====================================================================
//  IMU (LSM6DSOX via I2C, address 0x6A)
// =====================================================================
static constexpr uint8_t  IMU_ADDR      = 0x6A;
static constexpr uint32_t IMU_PERIOD_MS = 10;   // 100 Hz
static constexpr float    G0            = 9.80665f;
static constexpr float    RAD2DEG       = 57.295779513f;

// =====================================================================
//  SD CARD LOGGER
// =====================================================================
static constexpr uint32_t SD_FLUSH_MS  = 1000;
static constexpr size_t   LOG_BUF_SIZE = 8192;

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
// 500 kbps @ 16 MHz — Bosch MS4.3
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
RH_RF95 rf95(RFM95_CS, RFM95_INT);
Adafruit_LSM6DSOX imu;

// IMU state
bool     g_imuOk  = false;
float    g_gbx=0, g_gby=0, g_gbz=0;
uint32_t lastImuSend = 0;
uint32_t g_imuSeq   = 0;
float    g_ax=0, g_ay=0, g_az=1.0f;
float    g_gx=0, g_gy=0, g_gz=0;

// TX-side CAN snapshot (for gear calc + SD log + warning LEDs)
float   g_rpm      = 0;
float   g_veh_kph  = 0;
float   g_tps_pct  = 0;
float   g_map_kpa  = 0;
uint8_t g_gear     = 0;   // 0=neutral, 1-5=gear
float   g_clt_f    = 0;   // coolant temp (°F)
float   g_oil_psi  = 99;  // oil pressure (psi) — init high so no false warning at startup
float   g_fuel_psi = 99;  // fuel pressure (psi) — init high
float   g_steer_deg    = 0;    // steering angle (°), negative = left, positive = right
float   g_fuel_temp_c  = 0;    // fuel level sensor temp (°C) — PANW 103395-395 on A1
float    g_fuel_voltage  = 0;    // raw A1 voltage (V) — used for fuel low threshold
uint32_t g_fuelLowSince  = 0;    // millis() when voltage first dropped below threshold (0 = not low)
float   g_oil_temp1_c  = 0;    // oil temp 1 (°C) — from PE3 ECU via CAN (0x770 row 4)
float   g_oil_temp2_c  = 0;    // oil temp 2 (°C) — eBay sender on A3

// SD state
bool     g_sdOk      = false;
SdFat    sd;
SdFile   g_logFile;
char     g_logBuf[LOG_BUF_SIZE];
size_t   g_logPos    = 0;
uint32_t lastSdFlush = 0;

// LoRa sequence
uint32_t g_seq = 0;

// LED + status timing
uint32_t lastBlink    = 0;
uint32_t lastStatus   = 0;
uint32_t lastCanFrame = 0;
uint32_t lastGearCalc = 0;
uint32_t lastLedUpdate   = 0;
uint32_t lastSteerSample = 0;
uint32_t lastTempSample   = 0;
uint32_t lastAnalogTx     = 0;
uint32_t lastSdLog        = 0;
bool     ledState        = false;

// =====================================================================
//  HELPERS
// =====================================================================
static inline uint16_t u16_lohi(uint8_t lo, uint8_t hi) {
  return ((uint16_t)hi << 8) | lo;
}
static inline void put_u32_le(uint8_t *p, uint32_t v) {
  p[0]=(uint8_t)(v); p[1]=(uint8_t)(v>>8);
  p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static inline void put_i16_le(uint8_t *p, int16_t v) {
  p[0]=(uint8_t)((uint16_t)v); p[1]=(uint8_t)((uint16_t)v>>8);
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

void updateWarningLEDs() {
  // Engine warning (D7): coolant temp above 200°F
  setLed(ledEngWarn, (g_clt_f > CLT_WARN_F) ? COLOR_RED : COLOR_OFF);

  // Oil pressure warning (D6): oil pressure below 30 psi
  setLed(ledOilWarn, (g_oil_psi < OIL_WARN_PSI) ? COLOR_RED : COLOR_OFF);

  // Low fuel warning (D10): A1 below 1.64V continuously for 15s = fuel low
  uint32_t nowLed = millis();
  if (g_fuel_voltage < FUEL_LOW_THRESH_V) {
    if (g_fuelLowSince == 0) g_fuelLowSince = nowLed;
  } else {
    g_fuelLowSince = 0;
  }
  bool fuelLow = (g_fuelLowSince != 0) && ((nowLed - g_fuelLowSince) >= FUEL_LOW_DELAY_MS);
  setLed(ledFuelWarn, fuelLow ? COLOR_AMBER : COLOR_OFF);

  // Neutral light (D11): physical switch on PIN_NEUTRAL_SWITCH (active LOW)
  setLed(ledNeutral, (digitalRead(PIN_NEUTRAL_SWITCH) == LOW) ? COLOR_GREEN : COLOR_OFF);

  // Shift indicator (D9): blue when above shift RPM
  setLed(ledShift, (g_rpm >= SHIFT_RPM) ? COLOR_BLUE : COLOR_OFF);
}

// =====================================================================
//  BOSCH CAN DECODE (TX-side snapshot)
// =====================================================================
static constexpr float BAR_TO_PSI = 14.503774f;
static constexpr float PSI_TO_KPA = 6.894757f;

void updateTxSnapshot(const CanFrame &f) {
  if (f.ext) return;
  if (f.id == 0x770 && f.dlc >= 7) {
    g_rpm     = u16_lohi(f.d[1], f.d[2]) * 0.25f;
    g_veh_kph = u16_lohi(f.d[3], f.d[4]) * 0.00781f;
    g_tps_pct = f.d[5] * 0.391f;
    if (f.dlc >= 8) {
      uint8_t row = f.d[0];
      if (row == 3) g_clt_f       = (float)f.d[7] - 40.0f;  // coolant temp
      if (row == 4) g_oil_temp1_c = (float)f.d[7] - 39.0f;  // oil temp 1 (ECU)
    }
  } else if (f.id == 0x790 && f.dlc >= 2) {
    g_rpm = u16_lohi(f.d[0], f.d[1]);
    if (f.dlc >= 8) g_map_kpa = u16_lohi(f.d[6], f.d[7]) * 0.01f * PSI_TO_KPA;
  } else if (f.id == 0x771 && f.dlc >= 7 && f.d[0] == 0) {
    g_map_kpa = u16_lohi(f.d[5], f.d[6]) * 0.01f;
  } else if (f.id == 0x771 && f.dlc >= 8 && f.d[0] == 3) {
    g_fuel_psi = f.d[5] * 0.0515f * BAR_TO_PSI;  // fuel pressure
    g_oil_psi  = f.d[7] * 0.0513f * BAR_TO_PSI;  // oil pressure
  }
}

// =====================================================================
//  BOSCH SERIAL DEBUG DECODE
// =====================================================================
void printDecoded(const CanFrame &f) {
  if (f.ext) return;
  if (f.id == 0x770 && f.dlc >= 7) {
    uint8_t row = f.d[0];
    Serial.print(F("  -> RPM=")); Serial.print(u16_lohi(f.d[1],f.d[2])*0.25f,0);
    Serial.print(F(" VEH=")); Serial.print(u16_lohi(f.d[3],f.d[4])*0.00781f,1); Serial.print(F("kph"));
    Serial.print(F(" TPS=")); Serial.print(f.d[5]*0.391f,1); Serial.print('%');
    Serial.print(F(" IGN=")); Serial.print((int8_t)f.d[6]*1.365f,1); Serial.print(F("deg"));
    if (f.dlc>=8) {
      if (row==3) { Serial.print(F(" CLT=")); Serial.print(f.d[7]-40); Serial.print('F'); }
      if (row==4) { Serial.print(F(" OilT=")); Serial.print(f.d[7]-39); Serial.print('C'); }
      if (row==6) { Serial.print(F(" IAT=")); Serial.print(f.d[7]-40); Serial.print('C'); }
      if (row==9) { Serial.print(F(" Gear=")); Serial.print(f.d[7]); }
    }
    Serial.println();
  } else if (f.id == 0x771 && f.dlc >= 6) {
    uint8_t row = f.d[0];
    if (row==0&&f.dlc>=7) { Serial.print(F("  -> MAP=")); Serial.print(u16_lohi(f.d[5],f.d[6])*0.01f,1); Serial.println(F(" kPa")); }
    if (row==1&&f.dlc>=8) { Serial.print(F("  -> Batt=")); Serial.print(f.d[7]*0.111f,2); Serial.println('V'); }
    if (row==3&&f.dlc>=8) {
      Serial.print(F("  -> Fuel=")); Serial.print(f.d[5]*0.0515f*BAR_TO_PSI,1); Serial.print(F("psi"));
      Serial.print(F(" Oil=")); Serial.print(f.d[7]*0.0513f*BAR_TO_PSI,1); Serial.println(F("psi"));
    }
  } else if (f.id == 0x772 && f.dlc >= 3) {
    uint8_t row = f.d[0];
    if (row==0&&f.dlc>=6) {
      Serial.print(F("  -> L1=")); Serial.print(u16_lohi(f.d[2],f.d[3])*0.000244f,3);
      Serial.print(F(" L2=")); Serial.println(u16_lohi(f.d[4],f.d[5])*0.000244f,3);
    } else if (row==3) {
      Serial.print(F("  -> InjTime=")); Serial.print(f.d[2]*0.8192f,2); Serial.println(F(" ms"));
    } else {
      Serial.print(F("  -> 0x772 row=")); Serial.println(row);
    }
  } else if (f.id == 0x790) {
    if (f.dlc>=2) { Serial.print(F("  -> RPM=")); Serial.print(u16_lohi(f.d[0],f.d[1])); }
    if (f.dlc>=6) { Serial.print(F(" IGN=")); Serial.print(u16_lohi(f.d[4],f.d[5])*0.1f,1); Serial.print(F("deg")); }
    if (f.dlc>=8) { Serial.print(F(" MAP=")); Serial.print(u16_lohi(f.d[6],f.d[7])*0.01f*PSI_TO_KPA,1); Serial.print(F("kPa")); }
    Serial.println();
  }
}

// =====================================================================
//  LORA PACKET SENDERS
// =====================================================================
bool sendFrameBinary(const CanFrame &f) {
  uint8_t pkt[24] = {0};
  pkt[0]=0xA5; pkt[1]=0x01;
  uint8_t flags=0;
  if (f.ext) flags|=0x01;
  if (f.rtr) flags|=0x02;
  pkt[2]=flags; pkt[3]=(uint8_t)min<uint8_t>(f.dlc,8);
  put_u32_le(&pkt[4], f.id);
  put_u32_le(&pkt[8], (uint32_t)millis());
  for (uint8_t i=0;i<8;i++) pkt[12+i]=f.d[i];
  g_seq++;
  put_u32_le(&pkt[20], g_seq);
  canCsHigh();
  rf95.send(pkt, sizeof(pkt));
  rf95.waitPacketSent();
  return true;
}

void sendImuBinary(float ax, float ay, float az,
                   float gx, float gy, float gz) {
  uint8_t pkt[22] = {0};
  pkt[0]=0xA5; pkt[1]=0x02;
  g_imuSeq++;
  put_u32_le(&pkt[2], g_imuSeq);
  put_u32_le(&pkt[6], (uint32_t)millis());
  put_i16_le(&pkt[10], (int16_t)(ax * 1000.0f));
  put_i16_le(&pkt[12], (int16_t)(ay * 1000.0f));
  put_i16_le(&pkt[14], (int16_t)(az * 1000.0f));
  put_i16_le(&pkt[16], (int16_t)(gx * 10.0f));
  put_i16_le(&pkt[18], (int16_t)(gy * 10.0f));
  put_i16_le(&pkt[20], (int16_t)(gz * 10.0f));
  canCsHigh();
  rf95.send(pkt, sizeof(pkt));
  rf95.waitPacketSent();
}

// Packet type 0x03 — analog sensors: steering angle, oil temp 2 (external sender)
// Format: [0xA5][0x03][seq:4][ts:4][steer_ddeg:2][oil2_t_ddeg:2] = 14 bytes
// Values scaled as int16 × 10 (0.1° / 0.1°C resolution)
void sendAnalogBinary() {
  uint8_t pkt[14] = {0};
  static uint32_t analogSeq = 0;
  pkt[0] = 0xA5; pkt[1] = 0x03;
  analogSeq++;
  put_u32_le(&pkt[2], analogSeq);
  put_u32_le(&pkt[6], (uint32_t)millis());
  put_i16_le(&pkt[10], (int16_t)(g_steer_deg   * 10.0f));
  put_i16_le(&pkt[12], (int16_t)(g_oil_temp2_c * 10.0f));
  canCsHigh();
  rf95.send(pkt, sizeof(pkt));
  rf95.waitPacketSent();
}

// =====================================================================
//  SD CARD LOGGER
// =====================================================================
void sdInit() {
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  delay(250);

  if (!sd.begin(SdSpiConfig(SD_CS, SHARED_SPI, SD_SCK_MHZ(1)))) {
    Serial.print(F("SD: init failed — error code: 0x"));
    Serial.print(sd.sdErrorCode(), HEX);
    Serial.print(F("  data: 0x"));
    Serial.println(sd.sdErrorData(), HEX);
    g_sdOk = false;
    return;
  }

  char fname[16];
  for (uint16_t n = 1; n <= 9999; n++) {
    snprintf(fname, sizeof(fname), "LOG_%04u.CSV", n);
    if (!sd.exists(fname)) {
      g_logFile.open(fname, O_WRONLY | O_CREAT);
      break;
    }
  }

  if (!g_logFile.isOpen()) {
    Serial.println(F("SD: could not open log file"));
    g_sdOk = false;
    return;
  }

  g_logFile.println(F("ts_s,rpm,gear,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,steer_deg"));
  g_logFile.sync();
  g_sdOk = true;
  Serial.print(F("SD: logging to ")); Serial.println(fname);
}

void sdLog(uint32_t ts) {
  if (!g_sdOk) return;
  char row[80];
  int len = snprintf(row, sizeof(row),
    "%.3f,%.1f,%u,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f,%.2f\n",
    ts * 0.001f,
    g_rpm, (unsigned)g_gear,
    g_ax, g_ay, g_az,
    g_gx, g_gy, g_gz,
    g_steer_deg);
  if (len <= 0) return;
  if (g_logPos + (size_t)len >= LOG_BUF_SIZE) {
    g_logFile.write(g_logBuf, g_logPos);
    g_logPos = 0;
  }
  memcpy(g_logBuf + g_logPos, row, len);
  g_logPos += len;
}

void sdFlush() {
  if (!g_sdOk) return;
  if (g_logPos > 0) {
    g_logFile.write(g_logBuf, g_logPos);
    g_logPos = 0;
  }
  g_logFile.sync();
}

// =====================================================================
//  TEMPERATURE SENSORS
// =====================================================================

// PANW 103395-395: Beta equation  T = 1 / (1/T0 + (1/Beta)*ln(R/R0))
void readFuelTemp() {
  int raw = analogRead(PIN_FUEL_TEMP);
  float v_adc = raw * (3.3f / 4095.0f);
  g_fuel_voltage = v_adc;
  if (v_adc <= 0.01f || v_adc >= 3.29f) return;  // open/short circuit guard
  float r_therm = FUEL_R_PULLUP * v_adc / (3.3f - v_adc);
  float t_k = 1.0f / (1.0f / FUEL_T0_K + (1.0f / FUEL_BETA) * logf(r_therm / FUEL_R25));
  g_fuel_temp_c = t_k - 273.15f;
}

// eBay oil temp 2 sender: linear interpolation through calibration table
void readOilTemp2() {
  int raw = analogRead(PIN_OIL_TEMP2);
  float v_adc = raw * (3.3f / 4095.0f);
  if (v_adc <= 0.01f || v_adc >= 3.29f) return;  // open/short circuit guard
  float r_sender = OIL2_R_PULLUP * v_adc / (3.3f - v_adc);

  // Clamp to table edges
  if (r_sender >= OIL2_CAL_OHMS[0]) { g_oil_temp2_c = OIL2_CAL_TEMP[0]; return; }
  if (r_sender <= OIL2_CAL_OHMS[OIL2_CAL_POINTS - 1]) { g_oil_temp2_c = OIL2_CAL_TEMP[OIL2_CAL_POINTS - 1]; return; }

  // Find bracketing pair and interpolate
  for (int i = 0; i < OIL2_CAL_POINTS - 1; i++) {
    if (r_sender <= OIL2_CAL_OHMS[i] && r_sender >= OIL2_CAL_OHMS[i + 1]) {
      float frac = (r_sender - OIL2_CAL_OHMS[i]) / (OIL2_CAL_OHMS[i + 1] - OIL2_CAL_OHMS[i]);
      g_oil_temp2_c = OIL2_CAL_TEMP[i] + frac * (OIL2_CAL_TEMP[i + 1] - OIL2_CAL_TEMP[i]);
      return;
    }
  }
}

// =====================================================================
//  STEERING ANGLE
// =====================================================================
void readSteeringAngle() {
  int raw = analogRead(PIN_STEER);                      // 0–4095
  float v_adc  = raw * (3.3f / 4095.0f);               // ADC voltage (0–3.3V)
  float v_orig = v_adc * (5.0f / 3.3f);                // back-calc original pot voltage (0–5V)
  g_steer_deg  = 75.0f * (v_orig - 2.391f) / 1.045f + 8.0f;
}

// =====================================================================
//  SETUP
// =====================================================================
void setup() {
  pinMode(LED_RED, OUTPUT);
  digitalWrite(LED_RED, LOW);

  Serial.begin(115200);

  // 5-second startup delay — lets the 3.3V rail, IMU, and all peripherals
  // fully stabilise before any initialisation begins.
  delay(5000);

  // Warning LEDs — init all off
  pinMode(PIN_NEUTRAL_SWITCH, INPUT_PULLUP);
  pinMode(PIN_STEER,     INPUT);
  pinMode(PIN_FUEL_TEMP, INPUT);
  pinMode(PIN_OIL_TEMP2, INPUT);
  analogReadResolution(12);  // RP2040 12-bit ADC

  ledEngWarn.begin();  ledEngWarn.setBrightness(200);  ledEngWarn.clear();  ledEngWarn.show();
  ledOilWarn.begin();  ledOilWarn.setBrightness(200);  ledOilWarn.clear();  ledOilWarn.show();
  ledFuelWarn.begin(); ledFuelWarn.setBrightness(200); ledFuelWarn.clear(); ledFuelWarn.show();
  ledNeutral.begin();  ledNeutral.setBrightness(200);  ledNeutral.clear();  ledNeutral.show();
  ledShift.begin();    ledShift.setBrightness(200);    ledShift.clear();    ledShift.show();
  Serial.println(F("Warning LEDs: initialized"));

  SPI.setRX(PIN_MISO);
  SPI.setTX(PIN_MOSI);
  SPI.setSCK(PIN_SCK);
  SPI.begin();

  pinMode(RFM95_CS, OUTPUT); digitalWrite(RFM95_CS, HIGH);  // hold high before SD init shares SPI bus
  pinMode(CAN_CS,   OUTPUT); canCsHigh();
  pinMode(SD_CS,    OUTPUT); digitalWrite(SD_CS, HIGH);

  // LoRa reset
  pinMode(RFM95_RST, OUTPUT);
  digitalWrite(RFM95_RST, HIGH); delay(10);
  digitalWrite(RFM95_RST, LOW);  delay(10);
  digitalWrite(RFM95_RST, HIGH); delay(10);

  Serial.println(F("=== CAN->LoRa TX (binary) | Gear DAC | SD Logger | Warning LEDs ==="));

  sdInit();

  if (!rf95.init()) {
    Serial.println(F("ERROR: rf95.init() failed"));
    while (1) { digitalWrite(LED_RED, !digitalRead(LED_RED)); delay(100); }
  }
  if (!rf95.setFrequency(LORA_FREQ_MHZ)) {
    Serial.println(F("ERROR: setFrequency failed"));
    while (1) { digitalWrite(LED_RED, !digitalRead(LED_RED)); delay(100); }
  }
  rf95.setTxPower(LORA_TX_POWER_DBM, false);
  Serial.print(F("LoRa OK @ ")); Serial.print(LORA_FREQ_MHZ); Serial.println(F(" MHz"));

  bool canOk = mcpInit500k_16MHz();
  Serial.print(F("MCP2515 @500kbps: ")); Serial.println(canOk ? F("OK") : F("FAIL"));
  if (!canOk) {
    Serial.println(F("Check CAN wiring/CS/termination."));
    while (1) { digitalWrite(LED_RED, !digitalRead(LED_RED)); delay(200); }
  }

  Wire.begin();
  Wire.setClock(400000);

  // Retry up to 5 times with 50ms gaps in case the rail is still settling.
  for (uint8_t attempt = 0; attempt < 5 && !g_imuOk; attempt++) {
    g_imuOk = imu.begin_I2C(IMU_ADDR, &Wire);
    if (!g_imuOk) {
      Serial.print(F("IMU: init attempt ")); Serial.print(attempt + 1); Serial.println(F(" failed, retrying..."));
      delay(50);
    }
  }

  if (g_imuOk) {
    imu.setAccelRange(LSM6DS_ACCEL_RANGE_4_G);
    imu.setGyroRange(LSM6DS_GYRO_RANGE_250_DPS);
    imu.setAccelDataRate(LSM6DS_RATE_104_HZ);
    imu.setGyroDataRate(LSM6DS_RATE_104_HZ);

    Serial.println(F("IMU: calibrating gyro bias (1s — keep still)..."));
    uint32_t t0 = millis(); uint32_t n = 0;
    double sx=0, sy=0, sz=0;
    while (millis()-t0 < 1000) {
      sensors_event_t a, g, tmp;
      imu.getEvent(&a, &g, &tmp);
      sx += g.gyro.x * RAD2DEG;
      sy += g.gyro.y * RAD2DEG;
      sz += g.gyro.z * RAD2DEG;
      n++; delay(10);
    }
    if (n) { g_gbx=(float)(sx/n); g_gby=(float)(sy/n); g_gbz=(float)(sz/n); }
    Serial.print(F("IMU OK | gyro bias (dps): "));
    Serial.print(g_gbx,3); Serial.print(F(", "));
    Serial.print(g_gby,3); Serial.print(F(", ")); Serial.println(g_gbz,3);
  } else {
    Serial.println(F("IMU: LSM6DSOX not found at 0x6A after 5 attempts"));
  }

  dacWrite(GEAR_DAC[0]);
  Serial.println(F("MCP4725 DAC: initialized (Neutral voltage)"));

  lastCanFrame = millis();
  Serial.println(F("TX running."));
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

  // Periodic CAN status — every 5 s
  if (now - lastStatus >= 5000) {
    lastStatus = now;
    uint8_t eflg = mcpRead(0x2D);
    Serial.print(F("[STATUS] uptime=")); Serial.print(now/1000);
    Serial.print(F("s seq=")); Serial.print(g_seq);
    Serial.print(F(" gear=")); Serial.print(g_gear);
    Serial.print(F(" rpm=")); Serial.print(g_rpm,0);
    Serial.print(F(" kph=")); Serial.print(g_veh_kph,1);
    Serial.print(F(" ratio=")); Serial.print(g_veh_kph > 1.0f ? g_rpm/g_veh_kph : 0, 1);
    Serial.print(F(" clt=")); Serial.print(g_clt_f,1); Serial.print(F("F"));
    Serial.print(F(" oil=")); Serial.print(g_oil_psi,1); Serial.print(F("psi"));
    Serial.print(F(" fuel=")); Serial.print(g_fuel_psi,1); Serial.print(F("psi"));
    Serial.print(F(" steer="));    Serial.print(g_steer_deg,1);   Serial.print(F("deg"));
    Serial.print(F(" fuel_t="));   Serial.print(g_fuel_temp_c,1);  Serial.print(F("C"));
    Serial.print(F(" oil_t1="));  Serial.print(g_oil_temp1_c,1); Serial.print(F("C"));
    Serial.print(F(" oil_t2="));  Serial.print(g_oil_temp2_c,1); Serial.print(F("C"));
    Serial.print(F(" EFLG=0x")); Serial.print(eflg, HEX);
    if (eflg==0) Serial.print(F(" OK"));
    Serial.println();
  }

  // IMU sample + LoRa send at 100 Hz, SD log
  if (g_imuOk && (now - lastImuSend >= IMU_PERIOD_MS)) {
    lastImuSend = now;
    sensors_event_t a, g, tmp;
    imu.getEvent(&a, &g, &tmp);
    g_ax = a.acceleration.x / G0;
    g_ay = a.acceleration.y / G0;
    g_az = a.acceleration.z / G0;
    g_gx = g.gyro.x * RAD2DEG - g_gbx;
    g_gy = g.gyro.y * RAD2DEG - g_gby;
    g_gz = g.gyro.z * RAD2DEG - g_gbz;

    Serial.print(F("[IMU] ax=")); Serial.print(g_ax,3);
    Serial.print(F(" ay="));      Serial.print(g_ay,3);
    Serial.print(F(" az="));      Serial.print(g_az,3);
    Serial.print(F("g  gx="));    Serial.print(g_gx,1);
    Serial.print(F(" gy="));      Serial.print(g_gy,1);
    Serial.print(F(" gz="));      Serial.print(g_gz,1); Serial.println(F("dps"));

    sendImuBinary(g_ax, g_ay, g_az, g_gx, g_gy, g_gz);
  }

  // SD log at 50 Hz
  if (now - lastSdLog >= 20UL) {
    lastSdLog = now;
    sdLog(now);
  }

  // SD flush every second
  if (now - lastSdFlush >= SD_FLUSH_MS) {
    lastSdFlush = now;
    sdFlush();
  }

  // CAN timeout — zero stale values after 500ms with no frames
  if ((now - lastCanFrame) > 500UL) {
    g_rpm = 0; g_veh_kph = 0;
  }

  // Gear recalc + DAC update every 100ms
  if (now - lastGearCalc >= 100UL) {
    lastGearCalc = now;
    uint8_t newGear = calculateGear(g_rpm, g_veh_kph);
    Serial.print(F("[GCALC] rpm=")); Serial.print(g_rpm, 1);
    Serial.print(F(" kph=")); Serial.print(g_veh_kph, 2);
    Serial.print(F(" ratio="));
    Serial.print(g_veh_kph > 0.01f ? g_rpm / g_veh_kph : 0.0f, 1);
    Serial.print(F(" -> gear=")); Serial.println(newGear);
    if (newGear != g_gear) {
      g_gear = newGear;
      dacWrite(GEAR_DAC[g_gear]);
    }
  }

  // Steering angle sample at 50 Hz
  if (now - lastSteerSample >= STEER_PERIOD_MS) {
    lastSteerSample = now;
    readSteeringAngle();
  }

  // Fuel temp + oil temp sample at 4 Hz
  if (now - lastTempSample >= TEMP_PERIOD_MS) {
    lastTempSample = now;
    readFuelTemp();
    readOilTemp2();
  }

  // Transmit analog sensors (steering, fuel level temp, oil temp) at 10 Hz
  if (now - lastAnalogTx >= 100UL) {
    lastAnalogTx = now;
    sendAnalogBinary();
  }

  // Warning LED update every 100ms
  if (now - lastLedUpdate >= 100UL) {
    lastLedUpdate = now;
    updateWarningLEDs();
  }

  // Poll CAN RX flags
  uint8_t intf = mcpRead(CANINTF);
  CanFrame f;

  auto handleFrame = [&](uint8_t base, const char *label, uint8_t clearBit) {
    readRx(base, f);
    mcpBitMod(CANINTF, clearBit, 0x00);
    lastCanFrame = now;

    Serial.print(F("[TX] ")); Serial.print(label);
    Serial.print(F(" ID=0x")); Serial.print(f.id, HEX);
    Serial.print(F(" DLC=")); Serial.print(f.dlc);
    Serial.print(F(" DATA="));
    for (uint8_t i=0; i<f.dlc&&i<8; i++) {
      if (f.d[i]<16) Serial.print('0');
      Serial.print(f.d[i], HEX); Serial.print(' ');
    }
    Serial.println();
    printDecoded(f);
    updateTxSnapshot(f);
    sendFrameBinary(f);
  };

  if (intf & RX0IF) handleFrame(RXB0SIDH, "RX0", RX0IF);
  if (intf & RX1IF) handleFrame(RXB1SIDH, "RX1", RX1IF);
}
