// Feather RP2040 RFM95 915 MHz  — RadioHead TX test
#include <SPI.h>
#include <RH_RF95.h>

#define RFM95_CS   16
#define RFM95_INT  21   // DIO0
#define RFM95_RST  17
#define RF95_FREQ  915.0

RH_RF95 rf95(RFM95_CS, RFM95_INT);

void radioReset() {
  pinMode(RFM95_RST, OUTPUT);
  digitalWrite(RFM95_RST, HIGH); delay(10);
  digitalWrite(RFM95_RST, LOW);  delay(10);
  digitalWrite(RFM95_RST, HIGH); delay(10);
}

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\nRP2040 RFM95 TX test");

  radioReset();

  if (!rf95.init()) {
    Serial.println("LoRa init failed"); while (1) {}
  }
  rf95.setFrequency(RF95_FREQ);
  rf95.setTxPower(20, false);   // up to +20 dBm
  rf95.setSpreadingFactor(10);  // SF10 for range
  rf95.setSignalBandwidth(125000);

  Serial.println("LoRa ready (915.0 MHz). Sending packets...");
}

uint32_t n = 0;
void loop() {
  // fake “sensor” values
  float ax = 0.12f + 0.01f * (n % 50);   // g
  float ay = -0.05f;
  float tempC = 25.0f + (n % 10);

  char msg[64];
  snprintf(msg, sizeof(msg), "cnt=%lu,ax=%.2f,ay=%.2f,T=%.1f",
           (unsigned long)n, ax, ay, tempC);

  rf95.send((uint8_t*)msg, strlen(msg));
  rf95.waitPacketSent();

  Serial.print("TX -> "); Serial.println(msg);
  n++;
  delay(1000);
}
