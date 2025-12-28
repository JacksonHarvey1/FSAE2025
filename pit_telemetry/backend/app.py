"""Flask app providing a simple live dashboard and JSON API.

Endpoints:
  GET /            → HTML dashboard
  GET /api/latest  → latest telemetry JSON
  GET /api/health  → health/metrics JSON
  POST /api/run/start → start a new run (logs under runs/<run_id>/)
  POST /api/run/stop  → stop current run
  GET /stream      → Server-Sent Events (SSE) stream of latest JSON at ~10–20 Hz

Run locally for development::

    export FLASK_APP=pit_telemetry.backend.app
    flask run --host=0.0.0.0 --port=8000
"""

from __future__ import annotations

import json
import time
from typing import Any, Dict

from flask import Flask, Response, jsonify, render_template, request

try:
    # Package-relative imports when run as part of pit_telemetry (installed
    # or run via ``python -m pit_telemetry.backend.app``)
    from .config import settings
    from .ingest import SerialIngestor
except ImportError:  # pragma: no cover - fallback for ``python app.py``
    # When run as a standalone script from inside pit_telemetry/backend,
    # treat this directory as the import root so we can simply
    # ``import config`` and ``import ingest``.
    from config import settings  # type: ignore
    from ingest import SerialIngestor  # type: ignore


ingestor = SerialIngestor()
ingestor.start()

app = Flask(__name__, static_folder="static", template_folder="templates")


@app.route("/")
def index() -> str:
    return render_template("index.html")


@app.route("/graphs")
def graphs() -> str:
    """Page with live time-series graphs (RPM, MAP, temps, etc.)."""
    return render_template("graphs.html")


@app.route("/api/latest")
def api_latest() -> Response:
    latest = ingestor.latest()
    return jsonify(latest)


@app.route("/api/health")
def api_health() -> Response:
    s = ingestor.stats()
    data: Dict[str, Any] = {
        "serial_port": ingestor.port,
        "serial_baud": ingestor.baud,
        "serial_connected": ingestor.serial_connected(),
        "total_packets": s.total_packets,
        "drop_count": s.drop_count,
        "packet_rate_hz": s.packet_rate_hz(),
        "last_packet_age_s": s.last_packet_age_s(),
        "run_id": ingestor.current_run_id(),
    }
    return jsonify(data)


@app.route("/api/run/start", methods=["POST"])
def api_run_start() -> Response:
    payload = request.get_json(silent=True) or {}
    run_id = payload.get("run_id")
    run_id_str = ingestor.start_run(run_id=run_id)
    return jsonify({"status": "ok", "run_id": run_id_str})


@app.route("/api/run/stop", methods=["POST"])
def api_run_stop() -> Response:
    ingestor.stop_run()
    return jsonify({"status": "ok"})


@app.route("/stream")
def stream() -> Response:
    """Server-Sent Events endpoint pushing latest telemetry.

    The frontend connects with EventSource and receives JSON payloads.
    """

    def event_stream():
        while True:
            latest = ingestor.latest()
            if latest:
                yield "data: " + json.dumps(latest) + "\n\n"
            time.sleep(0.1)  # ~10 Hz

    return Response(event_stream(), mimetype="text/event-stream")


if __name__ == "__main__":
    # Simple dev server entrypoint
    app.run(host="0.0.0.0", port=8000, debug=True)
