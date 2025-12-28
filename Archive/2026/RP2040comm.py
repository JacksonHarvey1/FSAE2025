    """RP2040comm.py
    =================

    Read CAN frames from the RP2040 over USB serial and print
    lightly parsed engine data to the console.

    This expects the RP2040 to be running ``CAN_to_Pi_USB.ino`` which
    outputs lines of the form:

        CAN,<ts_ms>,<id_hex>,<ext>,<dlc>,<b0>,<b1>,<b2>,<b3>,<b4>,<b5>,<b6>,<b7>

    Any other lines (debug starting with "#" or "DBG") are ignored.

    Usage on the Pi::

        python3 RP2040comm.py /dev/ttyACM0

    If the port is omitted, ``/dev/ttyACM0`` is used by default.
    """

    import serial
    import time
    import sys
    from typing import Dict


    def _bytes_to_int(b: bytes, resolution: float = 1.0, signed: bool = True) -> float:
        """Convert up to 2 bytes (little‑endian) to an int and apply resolution."""

        raw = int.from_bytes(b, byteorder="little", signed=signed)
        return raw * resolution


    def parse_can_message(arbitration_id: int, data: bytes) -> Dict[str, float]:
        """Minimal AN400 CAN parser (adapted from dashboard data_parser.py).

        Given an arbitration ID and 8‑byte data payload, return a dict of
        parsed fields (RPM, MAP, Oil_Pressure, Battery_Voltage, etc.).
        """

        parsed: Dict[str, float] = {}

        # 0x0CFFF048: RPM, TPS, Fuel Open Time, Ignition Angle
        if arbitration_id == 0x0CFFF048:
            parsed["RPM"] = _bytes_to_int(data[0:2], resolution=1, signed=False)
            parsed["TPS"] = _bytes_to_int(data[2:4], resolution=0.1)
            parsed["Fuel_Open_Time"] = _bytes_to_int(data[4:6], resolution=0.1)
            parsed["Ignition_Angle"] = _bytes_to_int(data[6:8], resolution=0.1)

        # 0x0CFFF148: MAP (0.01 units/bit)
        elif arbitration_id == 0x0CFFF148:
            parsed["MAP"] = _bytes_to_int(data[0:2], resolution=0.01)

        # 0x0CFFF248: Oil Pressure + front shock pots (simplified)
        elif arbitration_id == 0x0CFFF248:
            # Oil pressure in bytes 2–3 (same formula as dashboard version)
            raw_op = int.from_bytes(data[2:4], byteorder="little", signed=True)
            parsed["Oil_Pressure_PSI"] = (raw_op / 1000.0) * 25.0 - 12.5

        # 0x0CFFF448: Wheel speeds FR/FL/BR/BL (0.2 Hz/bit)
        elif arbitration_id == 0x0CFFF448:
            parsed["WheelSpeed_FR_Hz"] = _bytes_to_int(data[0:2], resolution=0.2)
            parsed["WheelSpeed_FL_Hz"] = _bytes_to_int(data[2:4], resolution=0.2)
            parsed["WheelSpeed_BR_Hz"] = _bytes_to_int(data[4:6], resolution=0.2)
            parsed["WheelSpeed_BL_Hz"] = _bytes_to_int(data[6:8], resolution=0.2)

        # 0x0CFFF548: Battery Voltage, Engine Coolant, Air Temp
        elif arbitration_id == 0x0CFFF548:
            parsed["Battery_Voltage"] = _bytes_to_int(data[0:2], resolution=0.01)
            parsed["Engine_Coolant"] = _bytes_to_int(data[2:4], resolution=0.1)
            parsed["Air_Temp"] = _bytes_to_int(data[4:6], resolution=0.1)

        # 0x0CFFF848: Lambda & AFR
        elif arbitration_id == 0x0CFFF848:
            parsed["Lambda_Measured"] = _bytes_to_int(data[0:2], resolution=0.001)
            parsed["Lambda_2"] = _bytes_to_int(data[2:4], resolution=0.001)
            parsed["Target_Lambda"] = _bytes_to_int(data[4:6], resolution=0.001)
            parsed["Air_to_Fuel_Ratio"] = _bytes_to_int(data[6:8], resolution=0.1)

        return parsed


    def parse_can_serial_line(line: str) -> Dict[str, float]:
        """Parse one CAN line from the RP2040.

        Returns a dict of high‑level fields using ``parse_can_message`` if
        available. Otherwise returns an empty dict.
        """

        if not line.startswith("CAN,"):
            return {}

        parts = line.split(",")
        if len(parts) != 13:
            # malformed line
            return {}

        try:
            ts_ms = int(parts[1])  # currently unused, but parsed for completeness
            arb_id_str = parts[2]
            arb_id = int(arb_id_str, 16)
            ext = int(parts[3])  # 1 or 0, currently unused
            dlc = int(parts[4])
            bytes_raw = [int(b) & 0xFF for b in parts[5:13]]
        except ValueError:
            return {}

        if parse_can_message is None:
            return {}

        # Slice to DLC bytes; pad/truncate to 8 for safety
        data_bytes = bytearray(bytes_raw[:dlc] + [0] * (8 - dlc))
        return parse_can_message(arb_id, data_bytes)


    def main() -> None:
        if len(sys.argv) > 1:
            port = sys.argv[1]
        else:
            port = "/dev/ttyACM0"  # default; may be /dev/ttyACM1, etc.

        baud = 115200

        print(f"Opening {port} at {baud} baud ...")
        ser = serial.Serial(port, baud, timeout=1)
        time.sleep(1.0)

        # Optional: poke the RP2040 to prove the link both ways
        try:
            ser.write(b"PING\n")
        except Exception:
            pass

        latest_values: Dict[str, float] = {}

        print("Listening for CAN frames from RP2040 ... (Ctrl+C to quit)\n")
        while True:
            try:
                raw = ser.readline()
            except KeyboardInterrupt:
                print("\nExiting.")
                break

            if not raw:
                continue

            try:
                line = raw.decode(errors="ignore").strip()
            except Exception:
                continue

            if not line:
                continue

            # React to PING/PONG traffic or debug lines if desired
            if line == "PONG":
                print("[RP2040] PONG (serial OK)")
                # Example: toggle LED
                ser.write(b"LED ON\n")
                continue
            if line.startswith("#") or line.startswith("DBG"):
                # Debug / status from the RP2040
                print(line)
                continue

            parsed = parse_can_serial_line(line)
            if not parsed:
                # For development you can uncomment this to see unparsed lines
                # print("UNPARSED:", line)
                continue

            latest_values.update(parsed)

            # Very simple live textual display; you can customise this or
            # feed it into your dashboard code later.
            out = []
            if "RPM" in latest_values:
                out.append(f"RPM={latest_values['RPM']:.0f}")
            if "MAP" in latest_values:
                out.append(f"MAP={latest_values['MAP']:.2f}")
            if "Oil_Pressure_PSI" in latest_values:
                out.append(f"Oil={latest_values['Oil_Pressure_PSI']:.1f} PSI")
            if "Battery_Voltage" in latest_values:
                out.append(f"Batt={latest_values['Battery_Voltage']:.2f} V")
            if "Engine_Coolant" in latest_values:
                out.append(f"Coolant={latest_values['Engine_Coolant']:.1f}")
            if "Air_Temp" in latest_values:
                out.append(f"Air={latest_values['Air_Temp']:.1f}")

            if out:
                print(" | ".join(out))


    if __name__ == "__main__":
        main()
