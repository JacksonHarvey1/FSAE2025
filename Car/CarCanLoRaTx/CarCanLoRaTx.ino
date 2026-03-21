#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <SD.h>
#include <RH_RF95.h>
#include <Adafruit_LSM6DSOX.h>
#include <Adafruit_Sensor.h>

// =====================================================================
//  USER SETTINGS
// =====================================================================
static constexpr float   LORA_FREQ_MHZ     = 915.0;
static constexpr uint8_t LORA_TX_POWER_DBM = 20;

// SPI bus pins (Feather RP2040 RFM95)
static constexpr uint8_t PIN_MISO = 8;
static constexpr uint8_t PIN_SCK  = 14;
static constexpr uint8_t PIN_MOSI = 15;

// SPI chip selects
static constexpr uint8_t CAN_CS   = 5;
static constexpr uint8_t RFM95_CS = 16;
static constexpr uint8_t SD_CS    = 28;  // A2 on Feather RP2040

// RFM95 control pins
static constexpr uint8_t RFM95_INT = 21;
static constexpr uint8_t RFM95_RST = 17;

#ifndef PIN_LED
  #define PIN_LED LED_BUILTIN
#endif
static constexpr uint8_t LED_RED = PIN_LED;

// =====================================================================
//  MCP4725 DAC — Gear Indicator Output
// =====================================================================
// I2C address 0x60 (A0 pin = GND). Power VCC from 5V rail for 0-5V swing.
// The I2C data lines run at 3.3V which the MCP4725 accepts fine.
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
//  GEAR RATIO DETECTION
// =====================================================================
// Ratio = RPM / veh_kph at steady speed in each gear.
//
// HOW TO CALIBRATE:
//   Drive in each gear at steady RPM + speed.
//   Set GEAR_RATIOS[n-1] = measured_RPM / measured_KPH.
//   Example: 4000 RPM @ 36 kph in 1st → ratio = 111.1
//
// These defaults are estimates for a CBR600-based FSAE car.
// MUST be updated for your specific drivetrain.
static constexpr float GEAR_RATIOS[5] = {
  210.0f,  // 1st gear  (RPM per km/h)
  150.0f,  // 2nd gear
  110.0f,  // 3rd gear
   85.0f,  // 4th gear
   65.0f,  // 5th gear
};
static constexpr float VEH_MOVING_KPH = 5.0f;    // below this = Neutral
static constexpr float RPM_MIN         = 500.0f;  // below this = Neutral

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
// Logs to /LOG_XXXX.CSV on the SD card.
// CSV columns: ts_ms, rpm, veh_kph, gear, tps_pct, map_kpa,
//              ax_g, ay_g, az_g, gx_dps, gy_dps, gz_dps
static constexpr uint32_t SD_FLUSH_MS  = 1000;  // flush to SD every 1 second
static constexpr size_t   LOG_BUF_SIZE = 8192;  // 8 KB — holds ~1s at 100 Hz

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

// TX-side CAN snapshot (for gear calc + SD log)
float   g_rpm     = 0;
float   g_veh_kph = 0;
float   g_tps_pct = 0;
float   g_map_kpa = 0;
uint8_t g_gear    = 0;  // 0=neutral, 1-5=gear

// SD state
bool     g_sdOk      = false;
File     g_logFile;
char     g_logBuf[LOG_BUF_SIZE];
size_t   g_logPos    = 0;
uint32_t lastSdFlush = 0;

// LoRa sequence
uint32_t g_seq = 0;

// LED + status timing
uint32_t lastBlink  = 0;
uint32_t lastStatus = 0;
bool     ledState   = false;

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
// Fast-write: 2 bytes — [upper 4 bits | 0x0?, lower 8 bits]
void dacWrite(uint16_t val12) {
  val12 &= 0x0FFF;
  Wire.beginTransmission(DAC_ADDR);
  Wire.write((uint8_t)(val12 >> 8));   // bits [11:8]
  Wire.write((uint8_t)(val12 & 0xFF)); // bits [7:0]
  Wire.endTransmission();
}

// =====================================================================
//  GEAR DETECTION
// =====================================================================
// Returns 0 (neutral) or 1-5 (gear number).
uint8_t calculateGear(float rpm, float kph) {
  if (rpm < RPM_MIN || kph < VEH_MOVING_KPH) return 0; // Neutral

  float ratio = rpm / kph;

  // Find closest gear ratio using nearest-neighbour
  uint8_t best = 0;
  float bestDiff = fabsf(ratio - GEAR_RATIOS[0]);
  for (uint8_t i = 1; i < 5; i++) {
    float diff = fabsf(ratio - GEAR_RATIOS[i]);
    if (diff < bestDiff) { bestDiff = diff; best = i; }
  }
  return best + 1;  // 1-indexed
}

// =====================================================================
//  BOSCH CAN DECODE (TX-side snapshot for gear calc + SD logging)
// =====================================================================
static constexpr float BAR_TO_PSI = 14.503774f;
static constexpr float PSI_TO_KPA = 6.894757f;

void updateTxSnapshot(const CanFrame &f) {
  if (f.ext) return;
  if (f.id == 0x770 && f.dlc >= 7) {
    g_rpm     = u16_lohi(f.d[1], f.d[2]) * 0.25f;
    g_veh_kph = u16_lohi(f.d[3], f.d[4]) * 0.00781f;
    g_tps_pct = f.d[5] * 0.391f;
  } else if (f.id == 0x790 && f.dlc >= 2) {
    g_rpm = u16_lohi(f.d[0], f.d[1]);  // higher-res RPM
    if (f.dlc >= 8) g_map_kpa = u16_lohi(f.d[6], f.d[7]) * 0.01f * PSI_TO_KPA;
  } else if (f.id == 0x771 && f.dlc >= 7 && f.d[0] == 0) {
    g_map_kpa = u16_lohi(f.d[5], f.d[6]) * 0.01f;
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
      if (row==3) { Serial.print(F(" CLT=")); Serial.print(f.d[7]-40); Serial.print('C'); }
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
  put_i16_le(&pkt[10], (int16_t)(ax * 1000.0f));  // 0.001 g
  put_i16_le(&pkt[12], (int16_t)(ay * 1000.0f));
  put_i16_le(&pkt[14], (int16_t)(az * 1000.0f));
  put_i16_le(&pkt[16], (int16_t)(gx * 10.0f));    // 0.1 deg/s
  put_i16_le(&pkt[18], (int16_t)(gy * 10.0f));
  put_i16_le(&pkt[20], (int16_t)(gz * 10.0f));
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

  if (!SD.begin(SD_CS)) {
    Serial.println(F("SD: init failed — no card or wiring issue"));
    g_sdOk = false;
    return;
  }

  // Find next available log file: LOG_0001.CSV, LOG_0002.CSV ...
  char fname[16];
  for (uint16_t n = 1; n <= 9999; n++) {
    snprintf(fname, sizeof(fname), "/LOG_%04u.CSV", n);
    if (!SD.exists(fname)) {
      g_logFile = SD.open(fname, FILE_WRITE);
      break;
    }
  }

  if (!g_logFile) {
    Serial.println(F("SD: could not open log file"));
    g_sdOk = false;
    return;
  }

  // Write CSV header
  g_logFile.println(F("ts_ms,rpm,veh_kph,gear,tps_pct,map_kpa,"
                       "ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps"));
  g_logFile.flush();
  g_sdOk = true;
  Serial.print(F("SD: logging to ")); Serial.println(fname);
}

// Append one row to the in-RAM buffer. Flush to SD when buffer is nearly full.
void sdLog(uint32_t ts) {
  if (!g_sdOk) return;

  char row[120];
  int len = snprintf(row, sizeof(row),
    "%lu,%.1f,%.2f,%u,%.1f,%.2f,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f\n",
    (unsigned long)ts,
    g_rpm, g_veh_kph, (unsigned)g_gear,
    g_tps_pct, g_map_kpa,
    g_ax, g_ay, g_az,
    g_gx, g_gy, g_gz);

  if (len <= 0) return;

  // If buffer would overflow, flush first
  if (g_logPos + (size_t)len >= LOG_BUF_SIZE) {
    g_logFile.write((uint8_t*)g_logBuf, g_logPos);
    g_logPos = 0;
  }
  memcpy(g_logBuf + g_logPos, row, len);
  g_logPos += len;
}

void sdFlush() {
  if (!g_sdOk) return;
  if (g_logPos > 0) {
    g_logFile.write((uint8_t*)g_logBuf, g_logPos);
    g_logPos = 0;
  }
  g_logFile.flush();
}

// =====================================================================
//  SETUP
// =====================================================================
void setup() {
  pinMode(LED_RED, OUTPUT);
  digitalWrite(LED_RED, LOW);

  Serial.begin(115200);
  delay(600);

  SPI.setRX(PIN_MISO);
  SPI.setTX(PIN_MOSI);
  SPI.setSCK(PIN_SCK);
  SPI.begin();

  pinMode(CAN_CS, OUTPUT); canCsHigh();
  pinMode(SD_CS,  OUTPUT); digitalWrite(SD_CS, HIGH);

  // LoRa reset
  pinMode(RFM95_RST, OUTPUT);
  digitalWrite(RFM95_RST, HIGH); delay(10);
  digitalWrite(RFM95_RST, LOW);  delay(10);
  digitalWrite(RFM95_RST, HIGH); delay(10);

  Serial.println(F("=== CAN->LoRa TX (binary) | Gear DAC | SD Logger ==="));

  // LoRa init
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

  // CAN init @ 500 kbps
  bool canOk = mcpInit500k_16MHz();
  Serial.print(F("MCP2515 @500kbps: ")); Serial.println(canOk ? F("OK") : F("FAIL"));
  if (!canOk) {
    Serial.println(F("Check CAN wiring/CS/termination."));
    while (1) { digitalWrite(LED_RED, !digitalRead(LED_RED)); delay(200); }
  }

  // I2C for IMU + DAC
  Wire.begin();
  Wire.setClock(400000);

  // IMU init
  g_imuOk = imu.begin_I2C(IMU_ADDR, &Wire);
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
    Serial.println(F("IMU: LSM6DSOX not found at 0x6A"));
  }

  // DAC init — output 0 V (neutral)
  dacWrite(GEAR_DAC[0]);
  Serial.println(F("MCP4725 DAC: initialized (Neutral voltage)"));

  // SD card init
  sdInit();

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
    Serial.print(F("[TX] uptime=")); Serial.print(now/1000);
    Serial.print(F("s seq=")); Serial.print(g_seq);
    Serial.print(F(" gear=")); Serial.print(g_gear);
    Serial.print(F(" rpm=")); Serial.print(g_rpm,0);
    Serial.print(F(" kph=")); Serial.print(g_veh_kph,1);
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
    sdLog(now);  // 100 Hz log row
  }

  // SD flush every second
  if (now - lastSdFlush >= SD_FLUSH_MS) {
    lastSdFlush = now;
    sdFlush();
  }

  // Poll CAN RX flags
  uint8_t intf = mcpRead(CANINTF);
  CanFrame f;

  auto handleFrame = [&](uint8_t base, const char *label, uint8_t clearBit) {
    readRx(base, f);
    mcpBitMod(CANINTF, clearBit, 0x00);

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

    // Update TX snapshot + gear
    updateTxSnapshot(f);
    uint8_t newGear = calculateGear(g_rpm, g_veh_kph);
    if (newGear != g_gear) {
      g_gear = newGear;
      dacWrite(GEAR_DAC[g_gear]);
      Serial.print(F("[GEAR] -> ")); Serial.print(g_gear == 0 ? F("Neutral") : (String("Gear ")+g_gear).c_str());
      Serial.print(F("  DAC=")); Serial.println(GEAR_DAC[g_gear]);
    }

    sendFrameBinary(f);
  };

  if (intf & RX0IF) handleFrame(RXB0SIDH, "RX0", RX0IF);
  if (intf & RX1IF) handleFrame(RXB1SIDH, "RX1", RX1IF);
}
