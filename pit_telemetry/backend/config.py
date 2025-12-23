"""Configuration helpers for the pit telemetry backend.

All values can be overridden via environment variables when running on the Pi.
"""

from __future__ import annotations

import os
from dataclasses import dataclass


@dataclass
class Settings:
    # Serial
    serial_port: str = os.environ.get("FSAE_SERIAL_PORT", "/dev/ttyACM0")
    serial_baud: int = int(os.environ.get("FSAE_SERIAL_BAUD", "115200"))

    # Ingestion
    json_hz: float = float(os.environ.get("FSAE_JSON_HZ", "10"))

    # Paths
    base_dir: str = os.environ.get("FSAE_BASE_DIR", os.path.dirname(__file__))

    @property
    def runs_dir(self) -> str:
        return os.path.join(self.base_dir, "runs")


settings = Settings()

