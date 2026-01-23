# DynoRP2040_MCP2515Wing

Firmware + wiring notes for the dyno RP2040 node now that the original Feather RP2040 CAN board is dead and we are stacking a **Feather RP2040** with the **Adafruit FeatherWing MCP2515 CAN transceiver** (SPI).

The goal is to drop-in replace `DynoRP204CANHATINtegration` with this sketch so the Pi ingest stack keeps receiving the exact same NDJSON telemetry schema (~20 Hz) without any other changes.

---

## 1. Directory contents

| File | Purpose |
| --- | --- |
| `DynoRP2040_MCP2515Wing.ino` | Arduino sketch for Feather RP2040 + MCP2515 wing |
| `README.md` | This wiring + flashing guide |

---

## 2. Hardware overview

| Item | Notes |
| --- | --- |
| MCU | Adafruit Feather RP2040 (UF2 bootloader) |
| CAN Wing | Adafruit FeatherWing CAN (MCP2515 + TJA1051, product 5709) |
| CAN speed | 250 kbps (matches PE3 AN400) |
| Crystal | MCP2515 wing ships with 16 MHz crystal – do **not** swap |
| Bus type | Extended IDs only (0x0CFFF0xx etc.) |

Make sure CANH/CANL go to the dyno harness (PE3 ECU) and the system shares ground.

---

## 3. Pinout (RP2040 ↔ MCP2515 wing)

| Function | Feather RP2040 pin | Wing pad | Notes |
| --- | --- | --- | --- |
| SPI1 MISO | **GP8 / D22** | DO | Secondary SPI bus (spi1) |
| SPI1 MOSI | **GP15 / D23** | DI | |
| SPI1 SCK  | **GP14 / D24** | SCK | |
| MCP2515 CS | **GP5 / D5** | CS | Keep every other SPI device CS high |
| INT (optional) | none wired yet | INT | Sketch polls registers by default |
| STANDBY (optional) | none wired yet | STBY | Wing pulls this correctly |
| RESET (optional) | none wired yet | RST | Wing pulls this correctly |

These assignments come straight from the previous `Test1.ino` bring-up that we know works. If you rewire any of them, update the `PIN_CAN_*` defines near the top of the `.ino` and reflash.

---

## 4. Building / flashing

1. **Arduino IDE setup**
   - Board package: **Raspberry Pi RP2040** (via Boards Manager).
   - Board selection: `Adafruit Feather RP2040`.
   - Port: the UF2/serial device for your Feather.
2. **Libraries**
   - Sketch only needs stock `SPI.h`. All MCP2515 access is handwritten—no external library dependencies.
3. **Flash steps**
   - Open `DynoRP2040_MCP2515Wing.ino` in Arduino IDE.
   - Verify/Upload. On success the serial monitor (115200 baud) should show:
     ```
     # RP2040 + MCP2515 dyno bridge starting
     # Wiring summary ...
     # MCP2515 init 250k/16MHz => OK
     # CANSTAT=0x0
     ```
   - JSON lines will appear once CAN traffic is detected. Lines starting with `#` or `DBG` are status and can be ignored by the Pi.

---

## 5. Integration with the Pi ingest stack

Nothing changes on the Raspberry Pi side:

- The NDJSON schema (`ts_ms`, `pkt`, `rpm`, `tps_pct`, etc.) is identical to the old firmware.
- Serial parameters stay the same (115200 baud by default, bumpable if desired).
- `Dyno/dyno_ingest_influx.py`, `Dyno/Dyno_Final.py`, and the Docker stack continue to work without edits.

---

## 6. Troubleshooting cheatsheet

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| `# MCP2515 init 250k/16MHz => FAIL` | CS not actually on GP5, SPI pins swapped, or wing unpowered | Re-seat wing, check solder joints, ensure no other device pulls CS low |
| `# STAT ... RX overflow` | CAN bus active but wing can’t drain buffers fast enough | Check that INT pin isn’t floating if you enabled interrupt mode; confirm 250 kbps wiring; make sure loop is not blocked |
| JSON never prints | No CAN frames seen yet | Verify PE3 is powered and bus is 250 kbps extended; use `DBG RX ...` lines to confirm raw frames |
| Pi ingest misses packets | USB cable brown-out or serial baud mismatch | Keep 921600 on Pi if you crank up output rate; otherwise stay at 115200 |

---

## 7. Future tweaks

- If you later run a wire from the wing’s INT pad to an RP2040 GPIO, set `PIN_CAN_INT` in the sketch and flip `USE_CAN_INTERRUPT` to `1`. This lets the MCP2515 alert the MCU instead of being polled every loop.
- For different CAN bitrates, adjust `CNF1/2/3` in `mcpInit250k_16MHz()`.
- To log additional AN400 frames, extend `updateTelemetryFromFrame()` with new IDs just like the existing switch cases.

---

Keep this folder synced with the repo (git add/commit) so teammates flashing new boards don’t accidentally fall back to the old Feather-with-integrated-CAN firmware.
