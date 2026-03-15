# LoRa CAN Telemetry Tests

Companion sketches live in:
- `Testing/TransmitterCANTest/TransmitterCANTest.ino`
- `Testing/ReciverCANTest/ReciverCANTest.ino`

## Hardware assumptions
- Adafruit Feather RP2040 CAN (PID 5724) with RFM95 (RH_RF95 driver)
- SPI pins forced to the Feather defaults (MISO=8, MOSI=15, SCK=14) per existing test sketches
- 915 MHz LoRa frequency
- PE3 Bosch MS4.3 CAN bus at **500 kbps** (CNF1=0x01, CNF2=0x90, CNF3=0x02 on the MCP2515)

## Transmitter summary
- Simulates the PE3 Bosch MS4.3 snapshot (`rpm`, `veh_kph`, `map_kpa`, `lambda`, `fuel/oil pressure`, temps, etc.)
- Builds a 34 byte payload using the same scaling/conversions used in `Dyno/DynoRP204CANHATINtegration`
- Wraps payload with `[0xAA][version][seq][len]` header and sends every 10 ms (~100 Hz) over LoRa
- Serial output announces sequence number, RPM, TPS, MAP, lambda, oil pressure

## Receiver summary
- Listens on the same LoRa settings and validates header/version/length
- Decodes the packed payload back into floating-point engineering values
- Prints the entire snapshot with RSSI so you can compare against the transmitter log

## Basic test procedure
1. Flash `TransmitterCANTest.ino` onto one RP2040 Feather CAN board.
2. Flash `ReciverCANTest.ino` onto another board (or load sequentially if you only have one).
3. Open two Serial monitors at 115200 baud. You should see matching telemetry snapshots every ~10 ms (~100 Hz).
4. Compare sequence numbers and values to confirm packet integrity. RSSI provides link health information.

If you later hook up the real CAN decoder, the telemetry struct in the transmitter can be filled directly from live frames while keeping the LoRa packet layout unchanged.
