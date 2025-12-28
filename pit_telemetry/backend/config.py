"""Configuration helpers for the pit telemetry backend.

All values can be overridden via environment variables when running on the Pi.
"""

from __future__ import annotations

import os
from dataclasses import dataclass


@dataclass
class Settings:
    # Serial
    serial_port = "/dev/serial/by-id/usb-Adafruit_Feather_RP2040_CAN_DF641455DB3F1327-if00"
    serial_baud = serial_baud = 115200


    # Ingestion
    json_hz: float = float(os.environ.get("FSAE_JSON_HZ", "10"))

    # Paths
    base_dir: str = os.environ.get("FSAE_BASE_DIR", os.path.dirname(__file__))

    @property
    def runs_dir(self) -> str:
        return os.path.join(self.base_dir, "runs")


settings = Settings()

