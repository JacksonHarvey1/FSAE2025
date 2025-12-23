// Simple frontend that connects to /stream (SSE) and updates DOM elements.

function $(id) {
  return document.getElementById(id);
}

function setValue(id, value, digits) {
  const el = $(id);
  if (!el) return;
  if (value === undefined || value === null || Number.isNaN(value)) {
    el.textContent = "--";
    return;
  }
  if (typeof value === "number" && digits !== undefined) {
    el.textContent = value.toFixed(digits);
  } else {
    el.textContent = value;
  }
}

function updateFromTelemetry(t) {
  setValue("rpm", t.rpm, 0);
  setValue("tps_pct", t.tps_pct, 1);
  setValue("map_kpa", t.map_kpa, 1);
  setValue("lambda", t.lambda, 3);
  setValue("oil_psi", t.oil_psi, 1);
  setValue("coolant_c", t.coolant_c, 1);
  setValue("air_c", t.air_c, 1);
  setValue("batt_v", t.batt_v, 2);

  setValue("ws_fl_hz", t.ws_fl_hz, 1);
  setValue("ws_fr_hz", t.ws_fr_hz, 1);
  setValue("ws_bl_hz", t.ws_bl_hz, 1);
  setValue("ws_br_hz", t.ws_br_hz, 1);
}

function startSSE() {
  const statusEl = $("health-text");
  const es = new EventSource("/stream");

  es.onopen = () => {
    if (statusEl) statusEl.textContent = "Connected";
  };

  es.onerror = () => {
    if (statusEl) statusEl.textContent = "Disconnected (retrying...)";
  };

  es.onmessage = (evt) => {
    try {
      const data = JSON.parse(evt.data);
      updateFromTelemetry(data);
    } catch (e) {
      console.error("Bad telemetry JSON", e);
    }
  };
}

window.addEventListener("load", () => {
  startSSE();
});

