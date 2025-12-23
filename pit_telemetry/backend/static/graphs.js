// graphs.js - live time-series charts driven by the /stream SSE endpoint.

function makeChart(ctx, label, color) {
  return new Chart(ctx, {
    type: 'line',
    data: {
      labels: [],
      datasets: [{
        label,
        data: [],
        borderColor: color,
        backgroundColor: 'rgba(0,0,0,0)',
        borderWidth: 1,
        pointRadius: 0,
        tension: 0.1,
      }],
    },
    options: {
      animation: false,
      responsive: true,
      scales: {
        x: {
          title: { display: true, text: 'Time (s, relative)' },
          ticks: { maxTicksLimit: 6 },
        },
        y: {
          title: { display: true, text: label },
          ticks: { maxTicksLimit: 6 },
        },
      },
      plugins: {
        legend: { display: false },
      },
    },
  });
}

window.addEventListener('load', () => {
  const ctxRpm  = document.getElementById('chart_rpm')?.getContext('2d');
  const ctxMap  = document.getElementById('chart_map')?.getContext('2d');
  const ctxOil  = document.getElementById('chart_oil')?.getContext('2d');
  const ctxTemp = document.getElementById('chart_temp')?.getContext('2d');

  if (!ctxRpm || !ctxMap || !ctxOil || !ctxTemp) return;

  const chartRpm  = makeChart(ctxRpm,  'RPM',          'rgb(0, 200, 255)');
  const chartMap  = makeChart(ctxMap,  'MAP (kPa)',    'rgb(0, 255, 120)');
  const chartOil  = makeChart(ctxOil,  'Oil PSI',      'rgb(255, 200, 0)');
  const chartTemp = makeChart(ctxTemp, 'Temp (°C)',    'rgb(255, 80, 80)');

  const MAX_POINTS = 300; // roughly 30s if we sample ~10 Hz
  let t0 = null;

  function pushPoint(chart, tSec, value) {
    if (!chart || value === undefined || value === null || Number.isNaN(value)) return;
    const data = chart.data;
    data.labels.push(tSec);
    data.datasets[0].data.push(value);
    // Keep sliding window
    if (data.labels.length > MAX_POINTS) {
      data.labels.shift();
      data.datasets[0].data.shift();
    }
    chart.update('none');
  }

  const es = new EventSource('/stream');

  es.onmessage = (evt) => {
    try {
      const t = JSON.parse(evt.data);
      const ts = (t.ts_ms ?? null);
      if (ts == null) return;
      if (t0 === null) t0 = ts;
      const tSec = ((ts - t0) / 1000).toFixed(1);

      pushPoint(chartRpm,  tSec, typeof t.rpm === 'number' ? t.rpm : null);
      pushPoint(chartMap,  tSec, typeof t.map_kpa === 'number' ? t.map_kpa : null);
      pushPoint(chartOil,  tSec, typeof t.oil_psi === 'number' ? t.oil_psi : null);

      // For temp chart, prefer coolant but also show air if present
      const tempVal = typeof t.coolant_c === 'number'
        ? t.coolant_c
        : (typeof t.air_c === 'number' ? t.air_c : null);
      pushPoint(chartTemp, tSec, tempVal);
    } catch (e) {
      console.error('Bad telemetry JSON in graphs', e);
    }
  };
});

