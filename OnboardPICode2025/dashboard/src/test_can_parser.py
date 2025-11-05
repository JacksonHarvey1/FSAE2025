#!/usr/bin/env python3
"""
test_can_parser.py

Listen on can0 (SocketCAN) and print both raw and parsed CAN frames.
Initializes the CAN interface at start and cleans up on exit.
"""

import subprocess
import can
from data_parser import parse_can_message

def bring_up_interface(iface='can0', bitrate=250000):
    """Bring up the CAN interface with the given bitrate."""
    subprocess.run(
        ["sudo", "ip", "link", "set", iface, "up", "type", "can", "bitrate", str(bitrate)],
        check=True
    )
    print(f"{iface} up @ {bitrate}bps")

def bring_down_interface(iface='can0'):
    """Shut down the CAN interface."""
    subprocess.run(
        ["sudo", "ip", "link", "set", iface, "down"],
        check=True
    )
    print(f"{iface} down")

def main():
    try:
        # 1) Initialize CAN interface
        bring_up_interface('can0', 250000)

        # 2) Open SocketCAN bus
        bus = can.interface.Bus(channel='can0', bustype='socketcan')
        print("Listening on can0, Ctrl‑C to exit.")

        # 3) Read and decode frames
        while True:
            msg = bus.recv(timeout=1.0)
            if not msg:
                continue

            print("--------------------------------------------------")
            print(f"Arbitration ID: 0x{msg.arbitration_id:08X}")
            print(f"Raw Data     : {msg.data}")

            parsed = parse_can_message(msg.arbitration_id, msg.data)
            if parsed:
                for field, value in parsed.items():
                    print(f"{field:20s}: {value}")
            else:
                print(f"Unsupported CAN ID: 0x{msg.arbitration_id:08X}")

    except KeyboardInterrupt:
        print("\nUser interrupted, exiting...")
    finally:
        # 4) Clean up
        try:
            bus.shutdown()
        except NameError:
            pass
        bring_down_interface('can0')

if __name__ == "__main__":
    main()
