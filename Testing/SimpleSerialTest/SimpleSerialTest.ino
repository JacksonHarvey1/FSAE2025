// SimpleSerialTest.ino
// Minimal test sketch for RP2040 to verify USB serial communication with Raspberry Pi
// 
// This sketch does only one thing: send "HI {counter}" every second over USB serial
// The onboard LED blinks to confirm the sketch is running even if serial isn't working
//
// Upload this to your Adafruit Feather RP2040, then run simple_serial_test.py on the Pi

void setup() {
  // Initialize USB serial at 115200 baud
  Serial.begin(115200);
  
  // Configure onboard LED
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  
  // Wait a moment for serial to initialize
  delay(1000);
  
  // Send startup message
  Serial.println("# SimpleSerialTest starting...");
  Serial.println("# Adafruit Feather RP2040");
  Serial.println("# Baud: 115200");
  Serial.println("# Sending 'HI' every 1 second");
  Serial.println("# Lines starting with '#' are status messages");
  Serial.println("#");
}

void loop() {
  static unsigned long counter = 0;
  static unsigned long lastSendTime = 0;
  static bool ledState = false;
  
  unsigned long now = millis();
  
  // Send "HI {counter}" every 1000 milliseconds (1 second)
  if (now - lastSendTime >= 1000) {
    lastSendTime = now;
    counter++;
    
    // Send the message
    Serial.print("HI ");
    Serial.println(counter);
    
    // Toggle LED to show we're alive
    ledState = !ledState;
    digitalWrite(LED_BUILTIN, ledState);
  }
}
