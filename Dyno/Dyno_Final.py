#!/usr/bin/env python3
"""Dyno Final — RP2040 CAN telemetry dashboard

This script is an upgraded version of ``dyno_simple.py`` that keeps the robust
serial parsing loop but layers on a Dash/Plotly UI so you can tune the look and
feel in a browser. It can run directly on a laptop or the Raspberry Pi and is
meant for local-host usage (default: http://127.0.0.1:8050).

Key features
------------
* Reads newline-delimited JSON packets from the RP2040 CAN hat board
* Smoothly buffers the latest N seconds of data for plotting
* Serves a sleek dark UI with configurable metric selection
* Live time-series plot plus optional raw-value data table view
* Status banner that reflects serial connectivity / synthetic mode
* Optional synthetic data generator for UI development without hardware

Dependencies
------------
python -m pip install dash dash-bootstrap-components plotly pyserial
"""

from __future__ import annotations

import glob
import json
import math
import os
import threading
import time
import logging
from collections import deque
from dataclasses import dataclass
from typing import Any, Deque, Dict, List, Optional

try:
    import serial  # type: ignore
except ImportError:  # pragma: no cover
    serial = None

import dash
import dash_bootstrap_components as dbc
from dash import Dash, Input, Output, State, dash_table, dcc, html
import plotly.graph_objects as go

# ------------------ CONFIG ------------------
DEFAULT_PORT_PATTERNS = [
    "/dev/serial/by-id/usb-Adafruit_Feather_RP2040_CAN_*",
    "/dev/serial/by-id/usb-Adafruit_Feather_RP2040_*",
    "/dev/ttyACM*",
]
DEFAULT_PORT_FALLBACK = "/dev/ttyACM0"


def _resolve_serial_port() -> str:
    env_port = os.getenv("TELEM_PORT")
    if env_port:
        return env_port
    for pattern in DEFAULT_PORT_PATTERNS:
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[0]
    return DEFAULT_PORT_FALLBACK


PORT = _resolve_serial_port()
BAUD = int(os.getenv("TELEM_BAUD", "115200"))
DEFAULT_KEYS = ",".join(
    [
        "ts_ms","pkt","rpm","tps_pct","fot_ms","ign_deg",
        "rpm_rate_rps","tps_rate_pct_s","map_rate","maf_load_rate",
        "baro_kpa","map_kpa","lambda","lambda2","lambda_target",
        "batt_v","coolant_c","air_c","oil_psi",
        "ws_fl_hz","ws_fr_hz","ws_bl_hz","ws_br_hz",
        "therm5_temp","therm7_temp",
        "pwm_duty_pct_1","pwm_duty_pct_2","pwm_duty_pct_3","pwm_duty_pct_4",
        "pwm_duty_pct_5","pwm_duty_pct_6","pwm_duty_pct_7","pwm_duty_pct_8",
        "percent_slip","driven_wheel_roc","traction_desired_pct",
        "driven_avg_ws_ft_s","nondriven_avg_ws_ft_s",
        "ign_comp_deg","ign_cut_pct",
        "driven_ws1_ft_s","driven_ws2_ft_s",
        "nondriven_ws1_ft_s","nondriven_ws2_ft_s",
        "fuel_comp_accel_pct","fuel_comp_start_pct",
        "fuel_comp_air_pct","fuel_comp_coolant_pct",
        "fuel_comp_baro_pct","fuel_comp_map_pct",
        "ign_comp_air_deg","ign_comp_coolant_deg",
        "ign_comp_baro_deg","ign_comp_map_deg"
    ]
)
KEYS = [k.strip() for k in os.getenv("TELEM_KEYS", DEFAULT_KEYS).split(",") if k.strip()]

WINDOW_SECONDS = float(os.getenv("TELEM_WINDOW_S", "30"))
MAX_POINTS = int(os.getenv("TELEM_MAX_POINTS", "5000"))
RAW_TABLE_ROWS = int(os.getenv("TELEM_RAW_ROWS", "120"))

USE_FAKE_DATA = os.getenv("TELEM_FAKE_DATA", "0") == "1"
FAKE_RATE_HZ = float(os.getenv("TELEM_FAKE_RATE_HZ", "50"))

APP_HOST = os.getenv("DYNO_APP_HOST", "127.0.0.1")
APP_PORT = int(os.getenv("DYNO_APP_PORT", "8050"))
APP_DEBUG = os.getenv("DYNO_APP_DEBUG", "0") == "1"
THEME_NAME = os.getenv("DYNO_APP_THEME", "CYBORG")
THEME = getattr(dbc.themes, THEME_NAME, dbc.themes.CYBORG)
APP_REFRESH_MS = int(os.getenv("DYNO_APP_REFRESH_MS", "200"))

RATE_WINDOW = 5.0  # seconds for rolling rate estimate
STALE_THRESHOLD = 2.5  # show warning if no pkt within this window
DEBUG_SERIAL = True
# --------------------------------------------


@dataclass
class TelemetrySnapshot:
    timestamps: List[float]
    series: Dict[str, List[Optional[float]]]
    raw_rows: List[Dict[str, Any]]
    rate_hz: float
    last_packet_id: Optional[int]
    last_update_ts: Optional[float]
    total_samples: int
    start_ts: float


class TelemetryBuffer:
    def __init__(self, keys: List[str], window_s: float, max_points: int) -> None:
        self.keys = keys
        self.window_s = window_s
        self.max_points = max_points
        self.timestamps: Deque[float] = deque(maxlen=max_points)
        self.series: Dict[str, Deque[Optional[float]]] = {
            k: deque(maxlen=max_points) for k in keys
        }
        self.raw_history: Deque[Dict[str, Any]] = deque(maxlen=max_points)
        self.packet_times: Deque[float] = deque()
        self.last_packet_id: Optional[int] = None
        self.last_update_ts: Optional[float] = None
        self.total_samples: int = 0
        self.start_ts = time.time()
        self.lock = threading.Lock()

    def append(self, obj: Dict[str, Any]) -> None:
        now = time.time()
        row: Dict[str, Any] = {
            "pkt": obj.get("pkt"),
            "t_rel_s": now - self.start_ts,
            "t_abs_s": now,
        }
        with self.lock:
            self.timestamps.append(now)
            for key in self.keys:
                val = _to_float(obj.get(key))
                self.series[key].append(val)
                row[key] = val
            self.raw_history.append(row)

            self.packet_times.append(now)
            while self.packet_times and (now - self.packet_times[0]) > RATE_WINDOW:
                self.packet_times.popleft()

            self.last_packet_id = obj.get("pkt")
            self.last_update_ts = now
            self.total_samples += 1

            self._prune(now)

    def snapshot(self, max_rows: int) -> TelemetrySnapshot:
        with self.lock:
            ts = list(self.timestamps)
            series_copy = {k: list(v) for k, v in self.series.items()}
            raw_rows = list(self.raw_history)[-max_rows:]
            rate = self._compute_rate_locked()
            return TelemetrySnapshot(
                timestamps=ts,
                series=series_copy,
                raw_rows=raw_rows,
                rate_hz=rate,
                last_packet_id=self.last_packet_id,
                last_update_ts=self.last_update_ts,
                total_samples=self.total_samples,
                start_ts=self.start_ts,
            )

    def clear(self) -> None:
        with self.lock:
            self.timestamps.clear()
            for key in self.keys:
                self.series[key].clear()
            self.raw_history.clear()
            self.packet_times.clear()
            self.last_packet_id = None
            self.last_update_ts = None
            self.total_samples = 0
            self.start_ts = time.time()

    def _prune(self, now: float) -> None:
        while self.timestamps and (now - self.timestamps[0]) > self.window_s:
            self.timestamps.popleft()
            for key in self.keys:
                if self.series[key]:
                    self.series[key].popleft()

    def _compute_rate_locked(self) -> float:
        if len(self.packet_times) < 2:
            return 0.0
        span = self.packet_times[-1] - self.packet_times[0]
        if span <= 0:
            return 0.0
        return len(self.packet_times) / span


BUFFER = TelemetryBuffer(KEYS, WINDOW_SECONDS, MAX_POINTS)

_status_lock = threading.Lock()
_serial_status: Dict[str, Any] = {
    "mode": "synthetic" if USE_FAKE_DATA else "serial",
    "port": PORT,
    "baud": BAUD,
    "connected": False,
    "last_error": None,
    "last_connect_ts": None,
    "last_packet_ts": None,
    "last_line_ts": None,
    "last_line": None,
    "lines_total": 0,
    "lines_json": 0,
    "lines_bad_json": 0,
    "lines_ignored": 0,
}


def update_status(**kwargs: Any) -> None:
    with _status_lock:
        _serial_status.update(kwargs)


def get_status() -> Dict[str, Any]:
    with _status_lock:
        return dict(_serial_status)


def _to_float(value: Any) -> Optional[float]:
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def serial_worker() -> None:  # pragma: no cover - hardware side effect
    if serial is None:
        update_status(
            connected=False,
            last_error="pyserial not installed; run `pip install pyserial`",
        )
        return

    while True:
        try:
            ser = serial.Serial(PORT, BAUD, timeout=None)
            try:
                ser.reset_input_buffer()
            except Exception:
                pass
            update_status(connected=True, last_error=None, last_connect_ts=time.time())
            _read_loop(ser)
        except Exception as exc:  # reconnect loop
            update_status(connected=False, last_error=str(exc))
            time.sleep(1.0)
        finally:
            try:
                ser.close()
            except Exception:
                pass


def _read_loop(ser: "serial.Serial") -> None:
    last_pkt = None
    while True:
        raw = ser.read_until(b"\n")
        if not raw:
            continue
        line = raw.decode(errors="ignore").strip()
        now = time.time()
        update_status(
            last_line_ts=now,
            last_line=line[:160],
            lines_total=get_status().get("lines_total", 0) + 1,
        )
        if not line or line.startswith(("#", "DBG")) or not line.startswith("{"):
            update_status(lines_ignored=get_status().get("lines_ignored", 0) + 1)
            if line:
                print(f"[serial] ignored: {line}")
            continue
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            update_status(lines_bad_json=get_status().get("lines_bad_json", 0) + 1)
            print(f"[serial] bad json: {line}")
            continue
        update_status(lines_json=get_status().get("lines_json", 0) + 1)
        print(f"[serial] json: {line}")
        pkt = obj.get("pkt")
        if pkt is not None and pkt == last_pkt:
            continue
        last_pkt = pkt
        BUFFER.append(obj)
        update_status(last_packet_ts=now)


def fake_data_worker() -> None:
    phase = 0.0
    dt = 1.0 / max(FAKE_RATE_HZ, 1.0)
    update_status(connected=True, last_error=None, port="synthetic", baud=0)
    while True:
        now = time.time()
        obj = {
            "pkt": int(now * 1000) & 0xFFFF,
            "rpm": 2500 + 1200 * math.sin(phase),
            "tps_pct": 30 + 20 * math.sin(phase * 0.75 + 1.2),
            "map_kpa": 101 + 10 * math.cos(phase * 1.2),
            "batt_v": 12.5 + 0.2 * math.sin(phase * 0.5),
            "coolant_c": 85 + 8 * math.sin(phase * 0.33 + 0.2),
        }
        BUFFER.append(obj)
        update_status(last_packet_ts=time.time())
        phase += 0.1
        time.sleep(dt)


# -------------------- DASH UI --------------------
app: Dash = dash.Dash(
    __name__,
    external_stylesheets=[THEME],
    title="Dyno Final",
)

app.layout = dbc.Container(
    [
        html.H2("Dyno Final — RP2040 Live Telemetry", className="mt-3 mb-2"),
        html.Div(id="status-banner", className="mb-3"),
        dbc.Row(
            [
                dbc.Col(
                    dbc.Card(
                        [
                            dbc.CardHeader("Display options"),
                            dbc.CardBody(
                                [
                                    html.Label("Metrics"),
                                    dcc.Dropdown(
                                        id="metric-select",
                                        options=[{"label": k.upper(), "value": k} for k in KEYS],
                                        value=KEYS,
                                        multi=True,
                                        clearable=False,
                                    ),
                                    html.Div(
                                        [
                                            html.Label("Graph style", className="mt-3"),
                                            dcc.RadioItems(
                                                id="graph-style",
                                                options=[
                                                    {"label": " Lines", "value": "lines"},
                                                    {"label": " Lines + markers", "value": "lines+markers"},
                                                ],
                                                value="lines",
                                                inline=True,
                                            ),
                                        ]
                                    ),
                                    html.Div(
                                        [
                                            html.Label("Raw data view", className="mt-3"),
                                            dbc.Checklist(
                                                options=[{"label": " Show latest table", "value": "raw"}],
                                                value=["raw"],
                                                id="raw-toggle",
                                                switch=True,
                                            ),
                                        ]
                                    ),
                                    html.Button(
                                        "Reset buffer",
                                        id="reset-buffer",
                                        className="btn btn-outline-warning mt-3",
                                        n_clicks=0,
                                    ),
                                ]
                            ),
                        ]
                    ),
                    md=4,
                ),
                dbc.Col(
                    dbc.Card(
                        [
                            dbc.CardHeader("Live stats"),
                            dbc.CardBody(html.Div(id="stat-cards")),
                        ]
                    ),
                    md=8,
                ),
            ],
            className="mb-4",
        ),
        html.Div(
            dcc.Graph(id="live-graph", style={"display": "none"}),
            style={"display": "none"},
        ),
        dbc.Row(
            dbc.Col(
                html.Div(id="graph-grid"),
                width=12,
            ),
            className="mb-4",
        ),
        dbc.Row(
            dbc.Col(
                html.Div(
                    dash_table.DataTable(
                        id="raw-table",
                        data=[],
                        columns=[],
                        page_size=RAW_TABLE_ROWS,
                        style_table={"overflowX": "auto", "maxHeight": "420px"},
                        style_header={"backgroundColor": "#1b1f3a", "color": "#f8f9fa"},
                        style_cell={
                            "backgroundColor": "#0b0f25",
                            "color": "#f8f9fa",
                            "textAlign": "center",
                        },
                    ),
                    id="raw-table-container",
                ),
                width=12,
            ),
        ),
        dcc.Interval(id="update-interval", interval=APP_REFRESH_MS, n_intervals=0),
    ],
    fluid=True,
)


@app.callback(
    Output("live-graph", "figure"),
    Output("graph-grid", "children"),
    Output("stat-cards", "children"),
    Output("raw-table", "data"),
    Output("raw-table", "columns"),
    Output("raw-table-container", "style"),
    Output("status-banner", "children"),
    Input("update-interval", "n_intervals"),
    Input("metric-select", "value"),
    Input("graph-style", "value"),
    Input("raw-toggle", "value"),
)
def refresh_dashboard(_, selected_metrics, graph_style, raw_toggle):  # noqa: D401
    snapshot = BUFFER.snapshot(RAW_TABLE_ROWS)
    status = get_status()

    metrics = selected_metrics or KEYS
    legacy_fig = _build_legacy_overlay(snapshot, metrics, graph_style)
    graphs = _build_graph_grid(snapshot, metrics, graph_style)
    stats = _build_stat_cards(snapshot, metrics)
    table_data, table_columns = _build_table(snapshot)
    show_raw = raw_toggle and "raw" in raw_toggle
    table_style = {"display": "block"} if show_raw else {"display": "none"}
    banner = _build_status_banner(snapshot, status)

    return legacy_fig, graphs, stats, table_data, table_columns, table_style, banner


@app.callback(Output("metric-select", "value"), Input("reset-buffer", "n_clicks"))
def reset_buffer(n_clicks: int):
    if n_clicks:
        BUFFER.clear()
        return KEYS
    raise dash.exceptions.PreventUpdate


def _build_legacy_overlay(
    snapshot: TelemetrySnapshot, metrics: List[str], graph_style: str
) -> go.Figure:
    fig = go.Figure()
    ts = snapshot.timestamps
    if not ts or not metrics:
        fig.add_annotation(text="Waiting for data…", showarrow=False, font={"size": 18})
    else:
        x0 = ts[0]
        x = [t - x0 for t in ts]
        mode = "lines+markers" if graph_style == "lines+markers" else "lines"
        for key in metrics:
            series = snapshot.series.get(key)
            if not series:
                continue
            fig.add_trace(
                go.Scatter(
                    x=x,
                    y=series,
                    name=key.upper(),
                    mode=mode,
                    connectgaps=True,
                )
            )
        fig.update_xaxes(title_text="Time (s)")
        fig.update_yaxes(title_text="Value")
    fig.update_layout(
        template="plotly_dark",
        margin={"l": 40, "r": 20, "t": 30, "b": 40},
        legend={"orientation": "h", "y": -0.2},
        hovermode="x unified",
        transition_duration=200,
    )
    return fig


def _build_graph_grid(snapshot: TelemetrySnapshot, metrics: List[str], graph_style: str) -> List[dbc.Row]:
    ts = snapshot.timestamps
    if not ts or not metrics:
        return [dbc.Row(dbc.Col(dbc.Alert("Waiting for data…", color="secondary")))]

    x0 = ts[0]
    x = [t - x0 for t in ts]
    mode = "lines+markers" if graph_style == "lines+markers" else "lines"

    cols: List[dbc.Col] = []
    for key in metrics:
        series = snapshot.series.get(key)
        fig = _build_metric_figure(x, series, key, mode)
        cols.append(
            dbc.Col(
                dbc.Card(
                    [
                        dbc.CardHeader(key.upper()),
                        dbc.CardBody(
                            dcc.Graph(
                                figure=fig,
                                config={"displayModeBar": False},
                                style={"height": "280px"},
                            )
                        ),
                    ],
                    className="bg-dark text-light",
                ),
                xs=12,
                md=6,
            )
        )

    rows: List[dbc.Row] = []
    for i in range(0, len(cols), 2):
        rows.append(dbc.Row(cols[i : i + 2], className="g-3 mb-2"))
    return rows


def _build_metric_figure(
    x: List[float], series: Optional[List[Optional[float]]], key: str, mode: str
) -> go.Figure:
    fig = go.Figure()
    if not series:
        fig.add_annotation(text="No data", showarrow=False, font={"size": 16})
    else:
        fig.add_trace(
            go.Scatter(
                x=x,
                y=series,
                mode=mode,
                connectgaps=True,
            )
        )
    fig.update_xaxes(title_text="Time (s)")
    fig.update_yaxes(title_text=key.upper())
    fig.update_layout(
        template="plotly_dark",
        margin={"l": 40, "r": 10, "t": 10, "b": 40},
        showlegend=False,
        hovermode="x unified",
        transition_duration=150,
    )
    return fig


def _build_stat_cards(snapshot: TelemetrySnapshot, metrics: List[str]) -> List[dbc.Col]:
    cards: List[dbc.Col] = []
    last_row = snapshot.raw_rows[-1] if snapshot.raw_rows else None
    now = time.time()

    info_items = [
        ("Packets", f"{snapshot.total_samples}"),
        ("Rate", f"{snapshot.rate_hz:.1f} Hz"),
        (
            "Last update",
            "{:.1f}s ago".format(now - snapshot.last_update_ts)
            if snapshot.last_update_ts
            else "—",
        ),
    ]
    for label, value in info_items:
        cards.append(_stat_card(label, value))

    if last_row:
        for key in metrics:
            val = last_row.get(key)
            display = "—" if val is None else f"{val:.2f}"
            cards.append(_stat_card(key.upper(), display))
    return dbc.Row(cards, className="g-2")


def _stat_card(label: str, value: str) -> dbc.Col:
    return dbc.Col(
        dbc.Card(
            [
                dbc.CardBody(
                    [
                        html.Small(label, className="text-muted"),
                        html.H4(value, className="mb-0"),
                    ]
                )
            ],
            className="h-100 bg-dark text-light",
        ),
        xs=6,
        md=3,
        lg=2,
    )


def _build_table(snapshot: TelemetrySnapshot) -> tuple[List[Dict[str, Any]], List[Dict[str, str]]]:
    rows = []
    for row in snapshot.raw_rows:
        new_row = {"Time (s)": f"{row['t_rel_s']:.2f}"}
        for key in KEYS:
            val = row.get(key)
            new_row[key.upper()] = "—" if val is None else f"{val:.2f}"
        new_row["PKT"] = row.get("pkt")
        rows.append(new_row)
    columns = (
        [{"name": "Time (s)", "id": "Time (s)"}]
        + [{"name": key.upper(), "id": key.upper()} for key in KEYS]
        + [{"name": "PKT", "id": "PKT"}]
    )
    rows = rows[-RAW_TABLE_ROWS:]
    rows.reverse()  # show newest first
    return rows, columns


def _build_status_banner(snapshot: TelemetrySnapshot, status: Dict[str, Any]) -> dbc.Alert:
    now = time.time()
    connected = status.get("connected", False)
    mode = status.get("mode", "serial")
    last_error = status.get("last_error")
    last_packet_ts = status.get("last_packet_ts") or snapshot.last_update_ts

    if mode == "synthetic":
        color = "info"
        message = "Synthetic data generator active (TELEM_FAKE_DATA=1)"
    elif connected and last_packet_ts and (now - last_packet_ts) < STALE_THRESHOLD:
        color = "success"
        message = f"Serial connected @ {status.get('port')} ({status.get('baud')} baud)"
    elif connected:
        color = "warning"
        age = now - last_packet_ts if last_packet_ts else float("inf")
        message = f"Connected but no packets for {age:.1f}s"
        last_line = status.get("last_line")
        if last_line:
            message += f" | last line: {last_line}"
        message += (
            f" | total={status.get('lines_total', 0)} json={status.get('lines_json', 0)} "
            f"ignored={status.get('lines_ignored', 0)} bad={status.get('lines_bad_json', 0)}"
        )
    else:
        color = "danger"
        message = "Waiting for serial data…"
        if last_error:
            message += f" (last error: {last_error})"

    return dbc.Alert(message, color=color, className="mb-0")


# -------------------- ENTRY POINT --------------------
def main() -> None:
    if USE_FAKE_DATA:
        threading.Thread(target=fake_data_worker, daemon=True).start()
    else:
        threading.Thread(target=serial_worker, daemon=True).start()

    werkzeug_logger = logging.getLogger("werkzeug")
    werkzeug_logger.setLevel(logging.ERROR)
    werkzeug_logger.propagate = False
    app.logger.disabled = True

    app.run(
        host=APP_HOST,
        port=APP_PORT,
        debug=APP_DEBUG,
        dev_tools_silence_routes_logging=True,
    )


if __name__ == "__main__":
    main()
