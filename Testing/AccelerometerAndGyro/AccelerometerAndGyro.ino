#include <Wire.h>
#include <Adafruit_LSM6DSOX.h>
#include <Adafruit_Sensor.h>

Adafruit_LSM6DSOX imu;

static constexpr uint32_t BAUD = 115200;
static constexpr float G0 = 9.80665f;
static constexpr float RAD2DEG = 57.295779513f;

// Default address for Adafruit LSM6DSOX is usually 0x6A (DO low).
// If you tied DO high, use 0x6B.
static constexpr uint8_t IMU_ADDR = 0x6A;

static constexpr uint32_t PERIOD_MS   = 10;    // print period (~100 Hz)
static constexpr uint32_t CAL_MS      = 3000;  // 3s bias calibration
static constexpr uint32_t CAL_SKIP_MS = 200;   // settle time

float gbx_dps = 0.0f, gby_dps = 0.0f, gbz_dps = 0.0f; // gyro bias (deg/s)
uint32_t lastPrint = 0;

void calibrateGyroBias() {
  Serial.println("# Gyro bias calibration: keep sensor still...");
  delay(CAL_SKIP_MS);

  uint32_t t0 = millis();
  uint32_t n = 0;
  double sx = 0, sy = 0, sz = 0;

  while (millis() - t0 < CAL_MS) {
    sensors_event_t accel, gyro, temp;
    imu.getEvent(&accel, &gyro, &temp);

    float gx_dps = gyro.gyro.x * RAD2DEG;
    float gy_dps = gyro.gyro.y * RAD2DEG;
    float gz_dps = gyro.gyro.z * RAD2DEG;

    sx += gx_dps;
    sy += gy_dps;
    sz += gz_dps;
    n++;

    delay(5); // sample fast during calibration
  }

  if (n == 0) n = 1;
  gbx_dps = (float)(sx / n);
  gby_dps = (float)(sy / n);
  gbz_dps = (float)(sz / n);

  Serial.print("# Gyro bias (deg/s): ");
  Serial.print(gbx_dps, 5); Serial.print(", ");
  Serial.print(gby_dps, 5); Serial.print(", ");
  Serial.println(gbz_dps, 5);

  Serial.println("# Streaming: ts_ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,temp_C");
}

void setup() {
  Serial.begin(BAUD);
  delay(800);

  Serial.println("# serial alive");

  Wire.begin();
  Wire.setClock(100000);

  if (!imu.begin_I2C(IMU_ADDR, &Wire)) {
    Serial.println("# ERROR: LSM6DSOX not found at I2C address.");
    Serial.println("# Try changing IMU_ADDR to 0x6B if DO is tied high.");
    while (1) delay(1000);
  }

  imu.setAccelRange(LSM6DS_ACCEL_RANGE_4_G);
  imu.setGyroRange(LSM6DS_GYRO_RANGE_250_DPS);
  imu.setAccelDataRate(LSM6DS_RATE_104_HZ);
  imu.setGyroDataRate(LSM6DS_RATE_104_HZ);

  Serial.println("# LSM6DSOX OK");
  calibrateGyroBias();
}

void loop() {
  uint32_t now = millis();
  if (now - lastPrint < PERIOD_MS) return;
  lastPrint = now;

  sensors_event_t accel, gyro, temp;
  imu.getEvent(&accel, &gyro, &temp);

  // accel is m/s^2 -> g
  float ax_g = accel.acceleration.x / G0;
  float ay_g = accel.acceleration.y / G0;
  float az_g = accel.acceleration.z / G0;

  // gyro is rad/s -> deg/s, then subtract bias
  float gx_dps = gyro.gyro.x * RAD2DEG - gbx_dps;
  float gy_dps = gyro.gyro.y * RAD2DEG - gby_dps;
  float gz_dps = gyro.gyro.z * RAD2DEG - gbz_dps;

  Serial.print(now);
  Serial.print(",");
  Serial.print(ax_g, 5); Serial.print(",");
  Serial.print(ay_g, 5); Serial.print(",");
  Serial.print(az_g, 5); Serial.print(",");
  Serial.print(gx_dps, 5); Serial.print(",");
  Serial.print(gy_dps, 5); Serial.print(",");
  Serial.print(gz_dps, 5); Serial.print(",");
  Serial.println(temp.temperature, 2);
}
