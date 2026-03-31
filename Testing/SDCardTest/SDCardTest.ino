#include <SPI.h>
#include <SD.h>

#define SD_CS 28

void setup() {
  Serial.begin(115200);
  delay(600);

  // CS toggle test — put a multimeter on A2 and watch it blink
  pinMode(SD_CS, OUTPUT);
  for (int i = 0; i < 5; i++) {
    digitalWrite(SD_CS, LOW);  delay(200);
    digitalWrite(SD_CS, HIGH); delay(200);
  }
  Serial.println("CS toggle done — did A2 move on multimeter?");

  SPI.setRX(8);
  SPI.setTX(15);
  SPI.setSCK(14);
  SPI.begin();
  delay(200);

  Serial.println("Testing SD...");
  if (!SD.begin(SD_CS, SPI)) {
    Serial.println("FAIL - check wiring and FAT32 format");
    return;
  }

  File f = SD.open("/TEST.TXT", FILE_WRITE);
  if (!f) { Serial.println("FAIL - could not open file"); return; }
  f.println("SD works!");
  f.close();
  Serial.println("OK - TEST.TXT written");
}

void loop() {}
