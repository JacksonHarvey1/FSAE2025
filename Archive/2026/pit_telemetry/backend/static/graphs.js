// graphs.js - dynamic live time-series charts driven by the /stream SSE endpoint.

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

// Simple color palette for channels
const COLORS = [
  'rgb(0, 200, 255)',
  'rgb(0, 255, 120)',
  'rgb(255, 200, 0)',
  'rgb(255, 80, 80)',
  'rgb(200, 150, 255)',
  'rgb(150, 255, 200)',
  'rgb(255, 150, 150)',
  'rgb(200, 200, 200)',
];

function pickColor(index) {
  return COLORS[index % COLORS.length];
}

window.addEventListener('load', () => {
  const checkboxContainer = document.getElementById('channel-checkboxes');
  const chartsContainer = document.getElementById('charts-container');
  if (!checkboxContainer || !chartsContainer) return;

  const MAX_POINTS = 400; // sliding window (~20–40s depending on rate)
  let t0 = null;

  // Map: channel name -> { enabled, chart, labels, values, canvasDiv }
  const channels = new Map();

  function ensureChannelEntry(name) {
    if (!channels.has(name)) {
      channels.set(name, {
        enabled: false,
        chart: null,
        labels: [],
        values: [],
        canvasDiv: null,
      });
    }
    return channels.get(name);
  }

  function createCheckbox(name, idx) {
    const wrapper = document.createElement('label');
    wrapper.style.display = 'inline-block';
    wrapper.style.marginRight = '1rem';
    wrapper.style.marginBottom = '0.5rem';

    const cb = document.createElement('input');
    cb.type = 'checkbox';
    cb.value = name;

    const span = document.createElement('span');
    span.textContent = ` ${name}`;

    wrapper.appendChild(cb);
    wrapper.appendChild(span);
    checkboxContainer.appendChild(wrapper);

    const channel = ensureChannelEntry(name);

    cb.addEventListener('change', () => {
      channel.enabled = cb.checked;

      if (cb.checked) {
        // Create chart container + canvas
        if (!channel.canvasDiv) {
          const div = document.createElement('div');
          const title = document.createElement('h3');
          title.textContent = name;
          const canvas = document.createElement('canvas');
          canvas.width = 400;
          canvas.height = 200;

          div.appendChild(title);
          div.appendChild(canvas);
          chartsContainer.appendChild(div);

          const ctx = canvas.getContext('2d');
          channel.chart = makeChart(ctx, name, pickColor(idx));
          channel.canvasDiv = div;
        }
      } else {
        // Destroy chart & remove DOM
        if (channel.chart) {
          channel.chart.destroy();
          channel.chart = null;
        }
        if (channel.canvasDiv && channel.canvasDiv.parentNode) {
          channel.canvasDiv.parentNode.removeChild(channel.canvasDiv);
        }
        channel.canvasDiv = null;
        channel.labels = [];
        channel.values = [];
      }
    });
  }

  function buildChannelListFromSample(sample) {
    // Called once on first sample to populate checkboxes.
    const ignoreKeys = new Set(['ts_ms', 'pi_ts_ms', 'pkt', 'src', 'node_id']);
    const keys = Object.keys(sample)
      .filter((k) => !ignoreKeys.has(k) && typeof sample[k] === 'number')
      .sort();

    keys.forEach((name, idx) => {
      ensureChannelEntry(name);
      createCheckbox(name, idx);
    });
  }

  function pushPointToChannel(name, tSec, value) {
    const channel = channels.get(name);
    if (!channel || !channel.enabled || channel.chart == null) return;
    if (value === undefined || value === null || Number.isNaN(value)) return;

    channel.labels.push(tSec);
    channel.values.push(value);

    if (channel.labels.length > MAX_POINTS) {
      channel.labels.shift();
      channel.values.shift();
    }

    const chart = channel.chart;
    chart.data.labels = channel.labels.slice();
    chart.data.datasets[0].data = channel.values.slice();
    chart.update('none');
  }

  const es = new EventSource('/stream');
  let initializedChannels = false;

  es.onmessage = (evt) => {
    try {
      const t = JSON.parse(evt.data);
      const ts = t.ts_ms ?? null;
      if (ts == null) return;
      if (t0 === null) t0 = ts;
      const tSec = ((ts - t0) / 1000).toFixed(1);

      if (!initializedChannels) {
        buildChannelListFromSample(t);
        initializedChannels = true;
      }

      channels.forEach((info, name) => {
        const v = t[name];
        if (typeof v === 'number') {
          pushPointToChannel(name, tSec, v);
        }
      });
    } catch (e) {
      console.error('Bad telemetry JSON in graphs', e);
    }
  };
});
