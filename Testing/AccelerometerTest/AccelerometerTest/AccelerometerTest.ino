#include <Wire.h>
#include <Adafruit_LSM6DSOX.h>
#include <Adafruit_Sensor.h>

Adafruit_LSM6DSOX imu;

static const uint32_t SERIAL_BAUD = 115200;

// Change if needed (0x6A or 0x6B)
static const uint8_t  IMU_ADDR = 0x6A;

// Print rate
static const uint32_t PRINT_PERIOD_MS = 10; // 100 Hz prints. Use 20 for 50 Hz.

static uint32_t lastPrint = 0;

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(800);

  Serial.println("# serial alive");
  Serial.println("# ts_ms,ax_g,ay_g,az_g");

  Wire.begin();
  Wire.setClock(100000);

  if (!imu.begin_I2C(IMU_ADDR, &Wire)) {
    Serial.println("# ERROR: LSM6DSOX not found on I2C.");
    Serial.println("# Try addr 0x6B or run an I2C scanner.");
    while (1) delay(1000);
  }

  // Configure accel range + rate (pick what you want)
  // You said <=3g, so ±4g is a good default for headroom.
  imu.setAccelRange(LSM6DS_ACCEL_RANGE_4_G);
  imu.setGyroRange(LSM6DS_GYRO_RANGE_250_DPS); // gyro unused here but harmless
  imu.setAccelDataRate(LSM6DS_RATE_104_HZ);    // common stable rate
  imu.setGyroDataRate(LSM6DS_RATE_104_HZ);

  Serial.println("# LSM6DSOX OK");
}

void loop() {
  uint32_t now = millis();
  if (now - lastPrint < PRINT_PERIOD_MS) return;
  lastPrint = now;

  sensors_event_t accel;
  sensors_event_t gyro;
  sensors_event_t temp;

  // Adafruit Unified Sensor reports accel in m/s^2
  imu.getEvent(&accel, &gyro, &temp);

  const float G = 9.80665f;
  float ax_g = accel.acceleration.x / G;
  float ay_g = accel.acceleration.y / G;
  float az_g = accel.acceleration.z / G;

  Serial.print(now);
  Serial.print(",");
  Serial.print(ax_g, 4);
  Serial.print(",");
  Serial.print(ay_g, 4);
  Serial.print(",");
  Serial.println(az_g, 4);
}
