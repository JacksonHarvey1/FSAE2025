// CAN_to_Pi_USB_5724.ino
//
// Adafruit Feather RP2040 CAN (PID 5724)
//   → Sniff AN400 CAN frames from the PE3 ECU (250 kbps, extended IDs)
//   → Maintain a decoded telemetry snapshot
//   → Emit one NDJSON telemetry object over USB serial at ~20 Hz.
//
// NDJSON line format to the Pi (one JSON object per line, ~20 Hz):
//   { "ts_ms":..., "pkt":..., "src":"can", "node_id":1, ... }
//
// Lines starting with "#" are debug / status and can be ignored by the Pi ingestor.

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_MCP2515.h>

// ---------- HARD FAIL if wrong board selected ----------
#ifndef PIN_CAN_CS
  #error "Select Tools > Board: Adafruit Feather RP2040 CAN (so PIN_CAN_* matches the hardware)."
#endif

// ---------- Settings ----------
static constexpr uint32_t SERIAL_BAUD   = 115200;
static constexpr uint32_t CAN_BITRATE   = 250000;
static constexpr uint32_t JSON_PERIOD_MS = 50;   // 20 Hz
static constexpr uint32_t STAT_PERIOD_MS = 1000; // 1 Hz
static constexpr uint32_t BLINK_MS       = 500;  // LED heartbeat

// ---------- Board pins ----------
static constexpr uint8_t CAN_CS    = PIN_CAN_CS;
static constexpr uint8_t CAN_INT   = PIN_CAN_INTERRUPT;
static constexpr uint8_t CAN_STBY  = PIN_CAN_STANDBY;
static constexpr uint8_t CAN_RESET = PIN_CAN_RESET;

// Red LED on this Feather (D13)
#ifndef PIN_LED
  #define PIN_LED LED_BUILTIN
#endif
static constexpr uint8_t LED_RED = PIN_LED;

// ---------- CAN driver ----------
Adafruit_MCP2515 CAN(CAN_CS);

// ===== Telemetry snapshot (latest values seen on CAN) =====
static uint32_t g_pkt_counter     = 0;
static bool     g_seen_any_frame  = false;

static uint16_t g_rpm             = 0;
static float    g_tps_pct         = 0.0f;
static float    g_fot_ms          = 0.0f;
static float    g_ign_deg         = 0.0f;

static float    g_baro_kpa        = 0.0f;
static float    g_map_kpa         = 0.0f;
static float    g_lambda          = 0.0f;

static float    g_oil_psi         = 0.0f;   // project-specific mapping (PE3)
static float    g_batt_v          = 0.0f;
static float    g_coolant_c       = 0.0f;
static float    g_air_c           = 0.0f;

static float    g_ws_fl_hz        = 0.0f;
static float    g_ws_fr_hz        = 0.0f;
static float    g_ws_bl_hz        = 0.0f;
static float    g_ws_br_hz        = 0.0f;

// Helpers: AN400 is little-endian [LowByte, HighByte]
static inline uint16_t u16_lohi(uint8_t lo, uint8_t hi) {
  return (uint16_t)hi << 8 | lo;
}
static inline int16_t s16_lohi(uint8_t lo, uint8_t hi) {
  uint16_t n = u16_lohi(lo, hi);
  if (n > 32767) return (int16_t)(n - 65536);
  return (int16_t)n;
}

// Known PE IDs from AN400 (extended)
static const uint32_t ID_PE1 = 0x0CFFF048; // RPM, TPS, Fuel Open Time, Ign Angle
static const uint32_t ID_PE2 = 0x0CFFF148; // Barometer, MAP, Lambda, Pressure Type
static const uint32_t ID_PE3 = 0x0CFFF248; // Project-specific: Oil pressure etc.
static const uint32_t ID_PE5 = 0x0CFFF448; // Wheel speeds
static const uint32_t ID_PE6 = 0x0CFFF548; // Battery, Air Temp, Coolant
static const uint32_t ID_PE9 = 0x0CFFF848; // Lambda & AFR

// Update snapshot from a received CAN frame (expects extended frames)
void updateTelemetryFromFrame(uint32_t id, bool ext, uint8_t dlc, const uint8_t d[8]) {
  if (!ext) return;
  if (dlc < 8) return;

  g_seen_any_frame = true;

  if (id == ID_PE1) {
    g_rpm     = u16_lohi(d[0], d[1]);        // 1 rpm/bit
    g_tps_pct = s16_lohi(d[2], d[3]) * 0.1f; // %
    g_fot_ms  = s16_lohi(d[4], d[5]) * 0.1f; // ms
    g_ign_deg = s16_lohi(d[6], d[7]) * 0.1f; // deg
  }
  else if (id == ID_PE2) {
    float baro_raw = s16_lohi(d[0], d[1]) * 0.01f; // psi or kPa
    float map_raw  = s16_lohi(d[2], d[3]) * 0.01f; // psi or kPa
    float lam_raw  = s16_lohi(d[4], d[5]) * 0.01f; // lambda
    bool  kpa      = (d[6] & 0x01) != 0;           // pressure type bit

    if (kpa) {
      g_baro_kpa = baro_raw;
      g_map_kpa  = map_raw;
    } else {
      const float PSI_TO_KPA = 6.89476f;
      g_baro_kpa = baro_raw * PSI_TO_KPA;
      g_map_kpa  = map_raw  * PSI_TO_KPA;
    }
    g_lambda = lam_raw;
  }
  else if (id == ID_PE3) {
    // Your project-specific mapping:
    // Oil pressure encoded in bytes 2–3 with custom linear conversion.
    int16_t raw_op = s16_lohi(d[2], d[3]);
    g_oil_psi = (raw_op / 1000.0f) * 25.0f - 12.5f;
  }
  else if (id == ID_PE5) {
    // Wheel speeds in Hz (0.2 Hz/bit)
    g_ws_fr_hz = s16_lohi(d[0], d[1]) * 0.2f;
    g_ws_fl_hz = s16_lohi(d[2], d[3]) * 0.2f;
    g_ws_br_hz = s16_lohi(d[4], d[5]) * 0.2f;
    g_ws_bl_hz = s16_lohi(d[6], d[7]) * 0.2f;
  }
  else if (id == ID_PE6) {
    float batt_v  = s16_lohi(d[0], d[1]) * 0.01f; // volts
    float air_raw = s16_lohi(d[2], d[3]) * 0.1f;  // C or F
    float clt_raw = s16_lohi(d[4], d[5]) * 0.1f;  // C or F
    bool  temp_c  = (d[6] & 0x01) != 0;           // 0=F, 1=C

    g_batt_v = batt_v;

    if (temp_c) {
      g_air_c     = air_raw;
      g_coolant_c = clt_raw;
    } else {
      g_air_c     = (air_raw - 32.0f) * (5.0f / 9.0f);
      g_coolant_c = (clt_raw - 32.0f) * (5.0f / 9.0f);
    }
  }
  else if (id == ID_PE9) {
    g_lambda = s16_lohi(d[0], d[1]) * 0.01f; // lambda #1 measured
  }
}

// Emit one line of NDJSON telemetry representing the latest snapshot.
void sendTelemetryJson() {
  if (!g_seen_any_frame) return;

  g_pkt_counter++;
  uint32_t ts = millis();

  Serial.print('{');

  bool first = true;
  auto add_kv = [&](const char *key) {
    if (!first) Serial.print(',');
    first = false;
    Serial.print('"'); Serial.print(key); Serial.print('"');
    Serial.print(':');
  };

  add_kv("ts_ms");   Serial.print(ts);
  add_kv("pkt");     Serial.print(g_pkt_counter);
  add_kv("src");     Serial.print("\"can\"");
  add_kv("node_id"); Serial.print(1);

  add_kv("rpm");      Serial.print(g_rpm);
  add_kv("tps_pct");  Serial.print(g_tps_pct, 1);
  add_kv("fot_ms");   Serial.print(g_fot_ms, 1);
  add_kv("ign_deg");  Serial.print(g_ign_deg, 1);

  add_kv("baro_kpa"); Serial.print(g_baro_kpa, 2);
  add_kv("map_kpa");  Serial.print(g_map_kpa, 2);
  add_kv("lambda");   Serial.print(g_lambda, 3);

  add_kv("batt_v");     Serial.print(g_batt_v, 2);
  add_kv("coolant_c");  Serial.print(g_coolant_c, 1);
  add_kv("air_c");      Serial.print(g_air_c, 1);
  add_kv("oil_psi");    Serial.print(g_oil_psi, 1);

  add_kv("ws_fl_hz"); Serial.print(g_ws_fl_hz, 1);
  add_kv("ws_fr_hz"); Serial.print(g_ws_fr_hz, 1);
  add_kv("ws_bl_hz"); Serial.print(g_ws_bl_hz, 1);
  add_kv("ws_br_hz"); Serial.print(g_ws_br_hz, 1);

  Serial.println('}');
}

void setup() {
  pinMode(LED_RED, OUTPUT);
  digitalWrite(LED_RED, LOW);

  Serial.begin(SERIAL_BAUD);
  delay(800);

  Serial.println("# RP2040 CAN→USB bridge starting (PID 5724)");
  Serial.println("# Output: one JSON telemetry object per line (NDJSON), ~20 Hz");
  Serial.println("# Lines beginning with '#' are debug/status.");

  // Bring CAN transceiver out of standby + reset it
  pinMode(CAN_STBY, OUTPUT);
  digitalWrite(CAN_STBY, LOW);     // LOW = normal (not standby)

  pinMode(CAN_RESET, OUTPUT);
  digitalWrite(CAN_RESET, HIGH);
  delay(5);
  digitalWrite(CAN_RESET, LOW);
  delay(5);
  digitalWrite(CAN_RESET, HIGH);
  delay(10);

  pinMode(CAN_INT, INPUT_PULLUP);

  if (!CAN.begin(CAN_BITRATE)) {
    Serial.println("# ERROR: CAN init failed. Check board selection + library.");
    while (1) {
      digitalWrite(LED_RED, !digitalRead(LED_RED));
      delay(100);
    }
  }

  Serial.println("# CAN init OK @ 250k");
  Serial.print("# CAN pins: CS="); Serial.print(CAN_CS);
  Serial.print(" INT=");          Serial.print(CAN_INT);
  Serial.print(" STBY=");         Serial.print(CAN_STBY);
  Serial.print(" RESET=");        Serial.println(CAN_RESET);
}

uint32_t lastBlink = 0;
bool     ledState  = false;
uint32_t lastStat  = 0;
uint32_t lastJson  = 0;

uint32_t lastFrameCount = 0;
uint32_t frameCountThisSec = 0;

void loop() {
  uint32_t now = millis();

  // Heartbeat LED
  if (now - lastBlink >= BLINK_MS) {
    lastBlink = now;
    ledState = !ledState;
    digitalWrite(LED_RED, ledState);
  }

  // Read & decode CAN frames as they arrive
  int packetSize = CAN.parsePacket();
  if (packetSize > 0) {
    uint32_t id = CAN.packetId();
    bool ext = CAN.packetExtended();
    bool rtr = CAN.packetRtr();
    uint8_t dlc = (uint8_t)CAN.packetDlc();

    uint8_t data[8] = {0};
    if (!rtr) {
      uint8_t i = 0;
      while (CAN.available() && i < 8) {
        data[i++] = (uint8_t)CAN.read();
      }
    } else {
      // consume any bytes if present (shouldn't be for RTR)
      while (CAN.available()) (void)CAN.read();
    }

    frameCountThisSec++;

    // Update snapshot from known IDs
    if (!rtr) updateTelemetryFromFrame(id, ext, dlc, data);
  }

  // Periodic status (optional, 1 Hz)
  if (now - lastStat >= STAT_PERIOD_MS) {
    lastStat = now;
    Serial.print("# STAT frames/s=");
    Serial.println(frameCountThisSec);
    frameCountThisSec = 0;
  }

  // Periodic NDJSON telemetry snapshot (~20 Hz)
  if (now - lastJson >= JSON_PERIOD_MS) {
    lastJson = now;
    sendTelemetryJson();
  }
}
