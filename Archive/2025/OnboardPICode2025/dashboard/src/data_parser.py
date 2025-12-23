#!/usr/bin/env python3
# data_parser.py

"""
Parse raw 8‑byte CAN frames into human‑readable values.
"""

def bytes_to_int(b, resolution=1.0, signed=True):
    """Convert up to 2 bytes (little‑endian) to an int and apply resolution."""
    raw = int.from_bytes(b, byteorder="little", signed=signed)
    return raw * resolution

def parse_can_message(arbitration_id, data):
    """
    Given an arbitration ID and 8‑byte data payload,
    return a dict of parsed fields.
    """
    parsed = {}

    # 0x0CFFF048: RPM, TPS, Fuel Open Time, Ignition Angle
    if arbitration_id == 0x0CFFF048:
        parsed['RPM']             = bytes_to_int(data[0:2], resolution=1)
        parsed['TPS']             = bytes_to_int(data[2:4], resolution=0.1)
        parsed['Fuel_Open_Time']  = bytes_to_int(data[4:6], resolution=0.1)
        parsed['Ignition_Angle']  = bytes_to_int(data[6:8], resolution=0.1)

    # 0x0CFFF148: MAP (0.01 units/bit)
    elif arbitration_id == 0x0CFFF148:
        parsed['MAP'] = bytes_to_int(data[0:2], resolution=0.01)

    # 0x0CFFF248: Oil Pressure + Front Shock Pots
    elif arbitration_id == 0x0CFFF248:
        # Oil pressure in bytes 2–3
        raw_op = int.from_bytes(data[2:4], byteorder="little", signed=True)
        parsed['Oil_Pressure_PSI'] = (raw_op / 1000.0) * 25.0 - 12.5
        # Shock pot front‑right & front‑left (0.001 V/bit)
        parsed['Shock_Pot_FR_V']   = bytes_to_int(data[4:6], resolution=0.001)
        parsed['Shock_Pot_FL_V']   = bytes_to_int(data[6:8], resolution=0.001)

    # 0x0CFFF348: Rear Shock Pots (0.001 V/bit)
    elif arbitration_id == 0x0CFFF348:
        parsed['Shock_Pot_BR_V']   = bytes_to_int(data[0:2], resolution=0.001)
        parsed['Shock_Pot_BL_V']   = bytes_to_int(data[2:4], resolution=0.001)

    # 0x0CFFF448: Wheel Speeds FR/FL/BR/BL (0.2 Hz/bit)
    elif arbitration_id == 0x0CFFF448:
        parsed['WheelSpeed_FR_Hz'] = bytes_to_int(data[0:2], resolution=0.2)
        parsed['WheelSpeed_FL_Hz'] = bytes_to_int(data[2:4], resolution=0.2)
        parsed['WheelSpeed_BR_Hz'] = bytes_to_int(data[4:6], resolution=0.2)
        parsed['WheelSpeed_BL_Hz'] = bytes_to_int(data[6:8], resolution=0.2)

    # 0x0CFFF548: Battery Voltage, Engine Coolant, Air Temp
    elif arbitration_id == 0x0CFFF548:
        parsed['Battery_Voltage']  = bytes_to_int(data[0:2], resolution=0.01)
        parsed['Engine_Coolant']   = bytes_to_int(data[2:4], resolution=0.1)
        parsed['Air_Temp']         = bytes_to_int(data[4:6], resolution=0.1)

    # 0x0CFFF848: Lambda & AFR
    elif arbitration_id == 0x0CFFF848:
        parsed['Lambda_Measured']   = bytes_to_int(data[0:2], resolution=0.001)
        parsed['Lambda_2']          = bytes_to_int(data[2:4], resolution=0.001)
        parsed['Target_Lambda']     = bytes_to_int(data[4:6], resolution=0.001)
        parsed['Air_to_Fuel_Ratio'] = bytes_to_int(data[6:8], resolution=0.1)

    return parsed


if __name__ == "__main__":
    # Smoke‑test all branches
    tests = [
        (0x0CFFF048, bytearray([0xB8,0x0B, 0xD0,0x07, 0x96,0x09, 0x64,0x0A])),
        (0x0CFFF148, bytearray([0x05,0x90, 0,0, 0,0,0,0])),  # 0x9005=36869 → 368.69 *0.01=3.6869?
        (0x0CFFF248, bytearray([0,0,  0xF2,0x01, 0x09,0x00, 0x08,0x00])),
        (0x0CFFF348, bytearray([0x09,0x00, 0x08,0x00, 0,0,0,0])),
        (0x0CFFF448, bytearray([1,0, 2,0, 3,0, 4,0])),
        (0x0CFFF548, bytearray([0x3C,0x05, 0xA8,0x02, 0x86,0x02, 0,0])),
        (0x0CFFF848, bytearray([0xE8,0x03, 0x0A,0x04, 0xE8,0x03, 0x93,0x00])),
    ]
    for arb, raw in tests:
        print(f"ID=0x{arb:08X}, raw={list(raw)}")
        print(parse_can_message(arb, raw))
        print()
