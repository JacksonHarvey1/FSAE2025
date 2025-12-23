#include <SPI.h>
#include <RH_RF95.h>

#define RFM95_CS   16
#define RFM95_RST  17
#define RFM95_INT  21

#define RF95_FREQ  915.0   // use 915 MHz in the US

RH_RF95 rf95(RFM95_CS, RFM95_INT);

void resetRadio() {
  pinMode(RFM95_RST, OUTPUT);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);
  digitalWrite(RFM95_RST, LOW);
  delay(10);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);
}

void setup() {
  Serial.begin(115200);
  delay(800);

  Serial.println("=== RFM95 TX test ===");

  resetRadio();

  if (!rf95.init()) {
    Serial.println("❌ rf95.init() failed (check pins / radio present)");
    while (1) delay(100);
  }

  if (!rf95.setFrequency(RF95_FREQ)) {
    Serial.println("❌ setFrequency failed");
    while (1) delay(100);
  }

  // Optional: match your other node settings
  rf95.setTxPower(20, false);   // 2..20 typical; some libs allow 23 on RFM95

  // If you want explicit LoRa modem settings (optional):
  // bandwidth=125kHz, coding rate=4/5, spreading factor=SF7
  rf95.setModemConfig(RH_RF95::Bw125Cr45Sf128);

  Serial.println("✅ Radio init OK, transmitting...");
}

uint32_t counter = 0;

void loop() {
  char msg[64];
  snprintf(msg, sizeof(msg), "HELLO %lu", (unsigned long)counter++);

  Serial.print("TX: ");
  Serial.println(msg);

  rf95.send((uint8_t*)msg, strlen(msg));
  rf95.waitPacketSent();

  delay(1000);
}
