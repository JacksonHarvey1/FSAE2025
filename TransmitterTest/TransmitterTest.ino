// Feather RP2040 RFM95 - Simple TX (RadioHead RH_RF95)

#include <SPI.h>
#include <RH_RF95.h>

// Adafruit Feather RP2040 RFM (built-in RFM95) pin mapping
#define RFM95_CS   16
#define RFM95_INT  21
#define RFM95_RST  17

// Match this to your module band (915.0 in US, 868.0 EU, 433.0, etc.)
#define RF95_FREQ  915.0

RH_RF95 rf95(RFM95_CS, RFM95_INT);

uint32_t counter = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  pinMode(RFM95_RST, OUTPUT);
  digitalWrite(RFM95_RST, HIGH);

  // Hard reset the radio
  digitalWrite(RFM95_RST, LOW);
  delay(10);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);

  if (!rf95.init()) {
    Serial.println("LoRa radio init FAILED");
    while (1) delay(100);
  }
  Serial.println("LoRa radio init OK");

  if (!rf95.setFrequency(RF95_FREQ)) {
    Serial.println("setFrequency FAILED");
    while (1) delay(100);
  }
  Serial.print("Frequency set to "); Serial.println(RF95_FREQ);

  // Optional: set TX power (5..23 dBm). 13 is a nice safe start.
  rf95.setTxPower(13, false);

  // Optional: make sure both ends match if you change these later
  // rf95.setSpreadingFactor(7);
  // rf95.setSignalBandwidth(125000);
  // rf95.setCodingRate4(5);
}

void loop() {
  char msg[64];
  snprintf(msg, sizeof(msg), "hello from TX! count=%lu", (unsigned long)counter++);

  Serial.print("Sending: ");
  Serial.println(msg);

  rf95.send((uint8_t*)msg, strlen(msg));
  rf95.waitPacketSent();

  delay(1000);
}
