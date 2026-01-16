# LoRa CAN Telemetry Tests

Companion sketches live in:
- `Testing/TransmitterCANTest/TransmitterCANTest.ino`
- `Testing/ReciverCANTest/ReciverCANTest.ino`

## Hardware assumptions
- Adafruit Feather RP2040 CAN (PID 5724) with RFM95 (RH_RF95 driver)
- SPI pins forced to the Feather defaults (MISO=8, MOSI=15, SCK=14) per existing test sketches
- 915 MHz LoRa frequency

## Transmitter summary
- Simulates PE3 AN400 frames (RPM, TPS, FOT, IGN, MAP, BARO, lambda, oil pressure, battery, temps, wheel speeds)
- Builds a 34 byte payload using the same scaling/conversions used in `Dyno/DynoRP204CANHATINtegration`
- Wraps payload with `[0xAA][version][seq][len]` header and sends every 200 ms over LoRa
- Serial output announces sequence number, RPM, TPS, MAP, lambda, oil pressure

## Receiver summary
- Listens on the same LoRa settings and validates header/version/length
- Decodes the packed payload back into floating-point engineering values
- Prints the entire snapshot with RSSI so you can compare against the transmitter log

## Basic test procedure
1. Flash `TransmitterCANTest.ino` onto one RP2040 Feather CAN board.
2. Flash `ReciverCANTest.ino` onto another board (or load sequentially if you only have one).
3. Open two Serial monitors at 115200 baud. You should see matching telemetry snapshots every ~200 ms.
4. Compare sequence numbers and values to confirm packet integrity. RSSI provides link health information.

If you later hook up the real CAN decoder, the telemetry struct in the transmitter can be filled directly from live frames while keeping the LoRa packet layout unchanged.
