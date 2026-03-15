// CAN_to_Pi_USB_RP2040RFM_CANWing.ino
//
// Feather RP2040 RFM (5714) + CAN Bus FeatherWing (5709)
//   → Sniff Bosch MS4.3 (PE3 8400) CAN frames decoded from the PE3 ECU DBC
//   → Maintain a decoded telemetry snapshot
//   → Emit one NDJSON telemetry object over USB serial at ~20 Hz.
//
// NDJSON line format to the Pi (one JSON object per line, ~20 Hz):
//   { "ts_ms":..., "pkt":..., "src":"can", "node_id":1, ... }
//
// Lines starting with "#" are debug / status and can be ignored by the Pi ingestor.

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_LSM6DSOX.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_MCP2515.h>

// ---------- Settings ----------
static constexpr uint32_t SERIAL_BAUD    = 921600;  // set Serial Monitor to this
static constexpr uint32_t CAN_BITRATE    = 500000;
static constexpr uint32_t JSON_PERIOD_MS = 50;      // 20 Hz
static constexpr uint32_t STAT_PERIOD_MS = 1000;    // 1 Hz
static constexpr uint32_t BLINK_MS       = 500;     // LED heartbeat

// ---------- IMU settings ----------
static constexpr uint8_t  IMU_ADDR        = 0x6A; // LSM6DSOX default (SDO/SA0 low)
static constexpr float    G0              = 9.80665f;
static constexpr float    RAD2DEG         = 57.295779513f;
static constexpr uint32_t IMU_CAL_MS      = 3000;
static constexpr uint32_t IMU_CAL_SKIP_MS = 200;
#ifndef IMU_CALIBRATE_AT_BOOT
  #define IMU_CALIBRATE_AT_BOOT 1
#endif

// ---------- CAN FeatherWing pins (Adafruit CAN Bus FeatherWing 5709 defaults) ----------
static constexpr uint8_t CAN_CS  = 5;  // Feather pin 5
static constexpr uint8_t CAN_INT = 6;  // Feather pin 6

// LED
#ifndef PIN_LED
  #define PIN_LED LED_BUILTIN
#endif
static constexpr uint8_t LED_RED = PIN_LED;

// ---------- CAN + IMU drivers ----------
Adafruit_MCP2515 CAN(CAN_CS);
Adafruit_LSM6DSOX imu;

// ===== Telemetry snapshot (latest values seen on CAN) =====
static uint32_t g_pkt_counter     = 0;
static bool     g_seen_any_frame  = false;
static bool     g_imu_ok          = false;

static uint16_t g_rpm             = 0;
static float    g_veh_kph         = 0.0f;
static float    g_tps_pct         = 0.0f;
static float    g_ign_deg         = 0.0f;

static float    g_map_kpa         = 0.0f;
static float    g_map_ext_kpa     = 0.0f;
static float    g_lambda          = 0.0f;
static float    g_lambda2         = 0.0f;

static float    g_batt_v          = 0.0f;
static float    g_coolant_c       = 0.0f;
static float    g_oil_temp_c      = 0.0f;
static float    g_air_c           = 0.0f;
static uint8_t  g_gear            = 0;

static float    g_fuel_bar        = 0.0f;
static float    g_fuel_psi        = 0.0f;
static float    g_oil_bar         = 0.0f;
static float    g_oil_psi         = 0.0f;

// IMU snapshot
static float    g_ax_g            = 0.0f;
static float    g_ay_g            = 0.0f;
static float    g_az_g            = 0.0f;
static float    g_gx_dps          = 0.0f;
static float    g_gy_dps          = 0.0f;
static float    g_gz_dps          = 0.0f;
static float    g_imu_temp_c      = 0.0f;

// Gyro bias (deg/s)
static float    g_gbx_dps         = 0.0f;
static float    g_gby_dps         = 0.0f;
static float    g_gbz_dps         = 0.0f;

// Helpers: Bosch MS4.3 messages are little-endian [LowByte, HighByte]
static inline uint16_t u16_lohi(uint8_t lo, uint8_t hi) {
  return (uint16_t)hi << 8 | lo;
}
static inline int16_t s16_lohi(uint8_t lo, uint8_t hi) {
  uint16_t n = u16_lohi(lo, hi);
  if (n > 32767) return (int16_t)(n - 65536);
  return (int16_t)n;
}

static constexpr float BAR_TO_PSI = 14.503774f;
static constexpr float PSI_TO_KPA = 6.894757f;

// Bosch MS4.3 (PE3 8400) message IDs (standard 11-bit)
static const uint32_t ID_PE3_770 = 0x770;
static const uint32_t ID_PE3_771 = 0x771;
static const uint32_t ID_PE3_772 = 0x772;
static const uint32_t ID_PE3_790 = 0x790;

// Update snapshot from a received CAN frame (Bosch MS4.3 standard IDs)
void updateTelemetryFromFrame(uint32_t id, bool /*ext*/, uint8_t dlc, const uint8_t d[8]) {
  if (id == ID_PE3_770 && dlc >= 7) {
    g_seen_any_frame = true;
    uint8_t row = d[0];
    uint16_t raw_rpm = u16_lohi(d[1], d[2]);
    g_rpm = (uint16_t)((raw_rpm * 0.25f) + 0.5f);
    g_veh_kph = u16_lohi(d[3], d[4]) * 0.00781f;
    g_tps_pct = d[5] * 0.391f;
    g_ign_deg = (int8_t)d[6] * 1.365f;

    if (dlc >= 8) {
      if (row == 3) {
        g_coolant_c = d[7] - 40.0f;
      } else if (row == 4) {
        g_oil_temp_c = d[7] - 39.0f;
      } else if (row == 6) {
        g_air_c = d[7] - 40.0f;
      } else if (row == 9) {
        g_gear = d[7];
      }
    }
  }
  else if (id == ID_PE3_771 && dlc >= 6) {
    g_seen_any_frame = true;
    uint8_t row = d[0];
    if (row == 0 && dlc >= 7) {
      g_map_kpa = u16_lohi(d[5], d[6]) * 0.01f;
    } else if (row == 1 && dlc >= 8) {
      g_batt_v = d[7] * 0.111f;
    } else if (row == 3 && dlc >= 8) {
      g_fuel_bar = d[5] * 0.0515f;
      g_oil_bar  = d[7] * 0.0513f;
      g_fuel_psi = g_fuel_bar * BAR_TO_PSI;
      g_oil_psi  = g_oil_bar  * BAR_TO_PSI;
    }
  }
  else if (id == ID_PE3_772 && dlc >= 6) {
    g_seen_any_frame = true;
    if (d[0] == 0) {
      g_lambda  = u16_lohi(d[2], d[3]) * 0.000244f;
      g_lambda2 = u16_lohi(d[4], d[5]) * 0.000244f;
    }
  }
  else if (id == ID_PE3_790) {
    g_seen_any_frame = true;
    if (dlc >= 2) {
      g_rpm = u16_lohi(d[0], d[1]);
    }
    if (dlc >= 6) {
      g_ign_deg = u16_lohi(d[4], d[5]) * 0.1f;
    }
    if (dlc >= 8) {
      g_map_ext_kpa = u16_lohi(d[6], d[7]) * 0.01f * PSI_TO_KPA;
    }
  }
}

void calibrateImuGyroBias() {
  if (!g_imu_ok) return;

  Serial.println("# IMU gyro bias calibration: keep sensor still...");
  delay(IMU_CAL_SKIP_MS);

  uint32_t t0 = millis();
  uint32_t n = 0;
  double sx = 0.0, sy = 0.0, sz = 0.0;

  while (millis() - t0 < IMU_CAL_MS) {
    sensors_event_t accel, gyro, temp;
    imu.getEvent(&accel, &gyro, &temp);

    sx += (gyro.gyro.x * RAD2DEG);
    sy += (gyro.gyro.y * RAD2DEG);
    sz += (gyro.gyro.z * RAD2DEG);
    n++;
    delay(5);
  }

  if (n == 0) n = 1;
  g_gbx_dps = (float)(sx / n);
  g_gby_dps = (float)(sy / n);
  g_gbz_dps = (float)(sz / n);

  Serial.print("# IMU gyro bias (deg/s): ");
  Serial.print(g_gbx_dps, 5); Serial.print(", ");
  Serial.print(g_gby_dps, 5); Serial.print(", ");
  Serial.println(g_gbz_dps, 5);
}

void updateImuSnapshot() {
  if (!g_imu_ok) return;

  sensors_event_t accel, gyro, temp;
  imu.getEvent(&accel, &gyro, &temp);

  g_ax_g = accel.acceleration.x / G0;
  g_ay_g = accel.acceleration.y / G0;
  g_az_g = accel.acceleration.z / G0;

  g_gx_dps = (gyro.gyro.x * RAD2DEG) - g_gbx_dps;
  g_gy_dps = (gyro.gyro.y * RAD2DEG) - g_gby_dps;
  g_gz_dps = (gyro.gyro.z * RAD2DEG) - g_gbz_dps;

  g_imu_temp_c = temp.temperature;
}

// Emit one line of NDJSON telemetry representing the latest snapshot.
void sendTelemetryJson() {
  // Only emit JSON once we have actually received CAN traffic
  if (!g_seen_any_frame) return;

  g_pkt_counter++;
  uint32_t ts = millis();

  String s;
  s.reserve(480);

  s += "{\"ts_ms\":";
  s += ts;

  s += ",\"pkt\":";
  s += g_pkt_counter;

  s += ",\"src\":\"can\"";
  s += ",\"node_id\":1";

  s += ",\"rpm\":";
  s += g_rpm;

  s += ",\"veh_kph\":";
  s += String(g_veh_kph, 2);

  s += ",\"tps_pct\":";
  s += String(g_tps_pct, 1);

  s += ",\"ign_deg\":";
  s += String(g_ign_deg, 1);

  s += ",\"map_kpa\":";
  s += String(g_map_kpa, 2);

  s += ",\"map_ext_kpa\":";
  s += String(g_map_ext_kpa, 2);

  s += ",\"lambda\":";
  s += String(g_lambda, 3);

  s += ",\"lambda2\":";
  s += String(g_lambda2, 3);

  s += ",\"batt_v\":";
  s += String(g_batt_v, 2);

  s += ",\"coolant_c\":";
  s += String(g_coolant_c, 1);

  s += ",\"oil_temp_c\":";
  s += String(g_oil_temp_c, 1);

  s += ",\"air_c\":";
  s += String(g_air_c, 1);

  s += ",\"gear\":";
  s += String(g_gear);

  s += ",\"fuel_bar\":";
  s += String(g_fuel_bar, 2);
  s += ",\"fuel_psi\":";
  s += String(g_fuel_psi, 2);

  s += ",\"oil_bar\":";
  s += String(g_oil_bar, 2);
  s += ",\"oil_psi\":";
  s += String(g_oil_psi, 2);

  s += ",\"imu_ok\":";
  s += (g_imu_ok ? 1 : 0);

  s += ",\"ax_g\":";
  s += String(g_ax_g, 5);
  s += ",\"ay_g\":";
  s += String(g_ay_g, 5);
  s += ",\"az_g\":";
  s += String(g_az_g, 5);

  s += ",\"gx_dps\":";
  s += String(g_gx_dps, 5);
  s += ",\"gy_dps\":";
  s += String(g_gy_dps, 5);
  s += ",\"gz_dps\":";
  s += String(g_gz_dps, 5);

  s += ",\"imu_temp_c\":";
  s += String(g_imu_temp_c, 2);

  s += "}";
  Serial.println(s);
}

void setup() {
  pinMode(LED_RED, OUTPUT);
  digitalWrite(LED_RED, LOW);

  Serial.begin(SERIAL_BAUD);
  while (!Serial) delay(10);

  Serial.println("# RP2040 RFM + CAN FeatherWing starting");
  Serial.println("# Output: NDJSON (~20 Hz) once CAN traffic is seen");

  // Disable onboard LoRa radio (avoid SPI contention)
#ifdef PIN_RFM_CS
  pinMode(PIN_RFM_CS, OUTPUT);
  digitalWrite(PIN_RFM_CS, HIGH);
  Serial.println("# RFM CS forced HIGH (disabled)");
#endif

  // IMU setup (LSM6DSOX)
  Wire.begin();
  Wire.setClock(100000);
  if (!imu.begin_I2C(IMU_ADDR, &Wire)) {
    Serial.println("# WARN: LSM6DSOX not found. Continuing without IMU data.");
    g_imu_ok = false;
  } else {
    imu.setAccelRange(LSM6DS_ACCEL_RANGE_4_G);
    imu.setGyroRange(LSM6DS_GYRO_RANGE_250_DPS);
    imu.setAccelDataRate(LSM6DS_RATE_104_HZ);
    imu.setGyroDataRate(LSM6DS_RATE_104_HZ);
    g_imu_ok = true;
    Serial.println("# IMU init OK (LSM6DSOX @ 0x6A)");
#if IMU_CALIBRATE_AT_BOOT
    calibrateImuGyroBias();
#endif
  }

  pinMode(CAN_INT, INPUT_PULLUP);

  if (!CAN.begin(CAN_BITRATE)) {
    Serial.println("# ERROR: CAN init failed.");
    while (1) {
      digitalWrite(LED_RED, !digitalRead(LED_RED));
      delay(150);
    }
  }

  Serial.println("# CAN init OK @ 500k");
  Serial.print("# CAN pins: CS="); Serial.print(CAN_CS);
  Serial.print(" INT=");          Serial.println(CAN_INT);
}

uint32_t lastBlink = 0;
bool     ledState  = false;
uint32_t lastStat  = 0;
uint32_t lastJson  = 0;
uint32_t frameCountThisSec = 0;

void loop() {
  uint32_t now = millis();

  updateImuSnapshot();

  // Heartbeat LED
  if (now - lastBlink >= BLINK_MS) {
    lastBlink = now;
    ledState = !ledState;
    digitalWrite(LED_RED, ledState);
  }

  // Read & decode CAN frames
  int packetSize = CAN.parsePacket();
  if (packetSize > 0) {
    uint32_t id = CAN.packetId();
    bool ext = CAN.packetExtended();
    bool rtr = CAN.packetRtr();
    uint8_t dlc = (uint8_t)CAN.packetDlc();

    uint8_t data[8] = {0};
    if (!rtr) {
      uint8_t i = 0;
      while (CAN.available() && i < 8) data[i++] = (uint8_t)CAN.read();
    } else {
      while (CAN.available()) (void)CAN.read();
    }

    frameCountThisSec++;

    if (!rtr) updateTelemetryFromFrame(id, ext, dlc, data);
  }

  // 1 Hz status so you can see if frames exist at all
  if (now - lastStat >= STAT_PERIOD_MS) {
    lastStat = now;
    Serial.print("# STAT frames/s=");
    Serial.println(frameCountThisSec);
    frameCountThisSec = 0;
  }

  // 20 Hz NDJSON once CAN traffic has been seen
  if (now - lastJson >= JSON_PERIOD_MS) {
    lastJson = now;
    sendTelemetryJson();
  }
}
