// SD Card Test — Feather RP2040 RFM95
// Uses SdFat (same library as CarCanLoRaTx) to verify wiring and card format.
// Expected result in Serial Monitor (115200 baud):
//   SD init OK
//   File written: TEST0001.TXT
//   File read back OK: "SD test OK - <timestamp>ms"
//
// If it prints FAIL, check:
//   1. Card is FAT32 (use SD Card Formatter, not Windows format)
//   2. SPI wiring: MISO=8, MOSI=15, SCK=14, CS=A2(pin28)
//   3. SdFat library installed (Tools > Manage Libraries > "SdFat" by Bill Greiman)

#include <SPI.h>
#include <SdFat.h>

#define SD_CS 25  // D25 (GP25) on Feather RP2040

SdFat sd;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== SD Card Test (SdFat) ===");

  SPI.setRX(8);
  SPI.setTX(15);
  SPI.setSCK(14);
  SPI.begin();

  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  delay(250);  // SD power-up settling

  if (!sd.begin(SdSpiConfig(SD_CS, SHARED_SPI, SD_SCK_MHZ(1)))) {
    Serial.print("FAIL - SD init error code: 0x");
    Serial.print(sd.sdErrorCode(), HEX);
    Serial.print("  data: 0x");
    Serial.println(sd.sdErrorData(), HEX);
    Serial.println("Check: FAT32 format, wiring, card seated properly.");
    return;
  }
  Serial.println("SD init OK");

  // Write test file
  SdFile f;
  if (!f.open("TEST0001.TXT", O_WRONLY | O_CREAT | O_TRUNC)) {
    Serial.println("FAIL - could not create TEST0001.TXT");
    return;
  }
  char msg[48];
  snprintf(msg, sizeof(msg), "SD test OK - %lums\n", (unsigned long)millis());
  f.write(msg, strlen(msg));
  f.sync();
  f.close();
  Serial.println("File written: TEST0001.TXT");

  // Read it back to verify
  if (!f.open("TEST0001.TXT", O_RDONLY)) {
    Serial.println("FAIL - could not re-open TEST0001.TXT for read");
    return;
  }
  char buf[64] = {0};
  int n = f.read(buf, sizeof(buf) - 1);
  f.close();

  if (n > 0) {
    Serial.print("File read back OK: ");
    Serial.print(buf);
  } else {
    Serial.println("FAIL - read returned no data");
  }

  // Print card info
  uint32_t sizeMB = sd.card()->sectorCount() / 2048;
  Serial.print("Card size: ");
  Serial.print(sizeMB);
  Serial.println(" MB");
  Serial.println("=== Test complete ===");
}

void loop() {}
