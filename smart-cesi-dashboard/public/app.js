// ============================================================
// CONFIGURATION
// ============================================================
let thresholds = {
  temp:    { min: 20,  max: 26   },
  hum:     { min: 30,  max: 60   },
  gaz:     { max: 800            },
  pression:{ min: 980, max: 1030 }
};

let stats = { received: 0, errors: 0 };
let dataHistory = [];
let lastPresenceTime = null;
let seqCounter = 0;

// ============================================================
// WEBSOCKET
// ============================================================
let ws;
function connectWS() {
  ws = new WebSocket(`ws://${location.host}`);

  ws.onopen = () => {
    setStatus(true);
    addLog('Connexion WebSocket établie', 'ok');
  };

  ws.onclose = () => {
    setStatus(false);
    addLog('Connexion perdue — reconnexion dans 3s...', 'err');
    setTimeout(connectWS, 3000);
  };

  ws.onerror = () => {
    stats.errors++;
    document.getElementById('total-errors').textContent = stats.errors;
  };

  ws.onmessage = (event) => {
    try {
      const msg = JSON.parse(event.data);
      if (msg.type === 'status')  handleStatus(msg);
      if (msg.type === 'data')    handleData(msg.payload);
      if (msg.type === 'history') msg.data.forEach(d => updateCharts(d));
    } catch(e) {
      addLog('Erreur parsing message: ' + e.message, 'err');
    }
  };
}

// ============================================================
// STATUS
// ============================================================
function setStatus(connected) {
  const pill = document.getElementById('status-pill');
  const txt  = document.getElementById('status-text');
  pill.className = 'status-pill ' + (connected ? 'connected' : 'disconnected');
  txt.textContent = connected ? 'Connecté' : 'Déconnecté';
}

function handleStatus(msg) {
  setStatus(msg.connected);
  if (msg.port) document.getElementById('port-label').textContent = msg.port;
  if (msg.stats) {
    stats = msg.stats;
    document.getElementById('total-received').textContent = stats.totalReceived || 0;
    document.getElementById('total-errors').textContent   = stats.totalErrors   || 0;
  }
}

// ============================================================
// DATA HANDLER
// ============================================================
function handleData(data) {
  stats.received++;
  seqCounter++;

  document.getElementById('total-received').textContent = stats.received;
  document.getElementById('last-update').textContent    = new Date().toLocaleTimeString('fr-FR');
  document.getElementById('seq-value').textContent      = seqCounter;
  document.getElementById('last-json').textContent      = JSON.stringify(data, null, 2);

  // Température
  updateSensorCard('temp', data.temp, '°C',
    v => v < thresholds.temp.min || v > thresholds.temp.max,
    v => `${v.toFixed(1)}`,
    v => Math.min(100, Math.max(0, ((v - 10) / 40) * 100))
  );

  // Humidité
  updateSensorCard('hum', data.hum, '%',
    v => v < thresholds.hum.min || v > thresholds.hum.max,
    v => `${v.toFixed(1)}`,
    v => Math.min(100, Math.max(0, v))
  );

  // Pression
  updateSensorCard('pres', data.pression, 'hPa',
    v => v < thresholds.pression.min || v > thresholds.pression.max,
    v => `${v.toFixed(0)}`,
    v => Math.min(100, Math.max(0, ((v - 960) / 80) * 100))
  );

  // Gaz
  updateSensorCard('gaz', data.gaz, 'ADC',
    v => v > thresholds.gaz.max,
    v => `${v}`,
    v => Math.min(100, (v / 4095) * 100)
  );

  // Présence
  updatePresence(data.presence);

  // Alertes
  checkAlerts(data);

  // Historique + graphiques
  dataHistory.push(data);
  if (dataHistory.length > 60) dataHistory.shift();
  updateCharts(data);

  addLog(`[seq:${seqCounter}] T:${data.temp?.toFixed(1)}°C H:${data.hum?.toFixed(1)}% G:${data.gaz} P:${data.presence ? 'OUI' : 'non'}`, 'ok');
}

// ============================================================
// SENSOR CARD UPDATER
// ============================================================
function updateSensorCard(id, value, unit, isAlert, format, barPct) {
  if (value === undefined || value === null) return;
  const valEl    = document.getElementById(`${id}-value`);
  const barEl    = document.getElementById(`${id}-bar`);
  const statusEl = document.getElementById(`${id}-status`);
  const cardEl   = document.getElementById(`card-${id}`);

  valEl.textContent = format(value) + ' ' + unit;
  barEl.style.width = barPct(value) + '%';

  if (statusEl) {
    if (isAlert(value)) {
      statusEl.textContent = 'Alerte';
      statusEl.className   = 'sensor-status bad';
      cardEl.classList.add('alert');
      setTimeout(() => cardEl.classList.remove('alert'), 3000);
    } else {
      statusEl.textContent = 'Normal';
      statusEl.className   = 'sensor-status ok';
    }
  }
}

// ============================================================
// PRESENCE
// ============================================================
function updatePresence(val) {
  const circle  = document.getElementById('presence-circle');
  const label   = document.getElementById('presence-label');
  const badge   = document.getElementById('presence-badge');
  const btext   = document.getElementById('presence-text');
  const timeEl  = document.getElementById('presence-time');

  const occupied = val === 1 || val === true;

  circle.className = 'presence-circle ' + (occupied ? 'occupied' : 'empty');
  label.textContent = occupied ? 'Occupé' : 'Vide';
  badge.className   = 'presence-badge ' + (occupied ? 'occupied' : 'empty');
  btext.textContent = occupied ? 'FabLab occupé' : 'FabLab vide';

  if (occupied) {
    lastPresenceTime = new Date();
    timeEl.textContent = lastPresenceTime.toLocaleTimeString('fr-FR');
  }
}

// ============================================================
// ALERTS
// ============================================================
function checkAlerts(data) {
  const alerts = [];
  if (data.temp !== undefined) {
    if (data.temp < thresholds.temp.min) alerts.push(`Température trop basse : ${data.temp.toFixed(1)}°C (min ${thresholds.temp.min}°C)`);
    if (data.temp > thresholds.temp.max) alerts.push(`Température trop élevée : ${data.temp.toFixed(1)}°C (max ${thresholds.temp.max}°C)`);
  }
  if (data.hum !== undefined) {
    if (data.hum < thresholds.hum.min) alerts.push(`Humidité trop basse : ${data.hum.toFixed(1)}% (min ${thresholds.hum.min}%)`);
    if (data.hum > thresholds.hum.max) alerts.push(`Humidité trop élevée : ${data.hum.toFixed(1)}% (max ${thresholds.hum.max}%)`);
  }
  if (data.gaz !== undefined && data.gaz > thresholds.gaz.max) {
    alerts.push(`Qualité air dégradée : ${data.gaz} ADC (max ${thresholds.gaz.max})`);
  }
  if (data.pression !== undefined) {
    if (data.pression < thresholds.pression.min) alerts.push(`Pression trop basse : ${data.pression.toFixed(0)} hPa (min ${thresholds.pression.min})`);
    if (data.pression > thresholds.pression.max) alerts.push(`Pression trop élevée : ${data.pression.toFixed(0)} hPa (max ${thresholds.pression.max})`);
  }

  const banner = document.getElementById('alert-banner');
  const msgs   = document.getElementById('alert-messages');
  if (alerts.length > 0) {
    msgs.innerHTML = alerts.map(a => `<div>${a}</div>`).join('');
    banner.classList.remove('hidden');
    addLog('⚠ ' + alerts.join(' | '), 'warn');
  }
}

document.getElementById('alert-close').addEventListener('click', () => {
  document.getElementById('alert-banner').classList.add('hidden');
});

// ============================================================
// CHARTS
// ============================================================
const chartConfig = (label, color, data) => ({
  type: 'line',
  data: {
    labels: Array(60).fill(''),
    datasets: [{
      label,
      data,
      borderColor: color,
      backgroundColor: color + '15',
      borderWidth: 1.5,
      pointRadius: 0,
      fill: true,
      tension: 0.3
    }]
  },
  options: {
    responsive: true,
    maintainAspectRatio: false,
    animation: { duration: 200 },
    plugins: { legend: { display: false } },
    scales: {
      x: { display: false },
      y: {
        grid: { color: '#f0f0ec' },
        ticks: { font: { family: 'DM Mono', size: 11 }, color: '#9b9b93' }
      }
    }
  }
});

const tempData = [], humData = [], gazData = [], presData = [];

const tempChart = new Chart(document.getElementById('tempChart'), chartConfig('Température', '#e85d3a', tempData));
const humChart  = new Chart(document.getElementById('humChart'),  chartConfig('Humidité',    '#3a8fd4', humData));
const gazChart  = new Chart(document.getElementById('gazChart'),  chartConfig('Qualité air', '#d4943a', gazData));
const presChart = new Chart(document.getElementById('presChart'), chartConfig('Pression',    '#7c5cbf', presData));

function updateCharts(data) {
  const pushAndShift = (arr, val) => {
    if (val !== undefined && val !== null) arr.push(val);
    if (arr.length > 60) arr.shift();
  };
  pushAndShift(tempData, data.temp);
  pushAndShift(humData,  data.hum);
  pushAndShift(gazData,  data.gaz);
  pushAndShift(presData, data.pression);

  tempChart.update('none');
  humChart.update('none');
  gazChart.update('none');
  presChart.update('none');
}

// ============================================================
// SETTINGS
// ============================================================
document.getElementById('btn-apply').addEventListener('click', () => {
  thresholds.temp.min     = parseFloat(document.getElementById('s-temp-min').value);
  thresholds.temp.max     = parseFloat(document.getElementById('s-temp-max').value);
  thresholds.hum.min      = parseFloat(document.getElementById('s-hum-min').value);
  thresholds.hum.max      = parseFloat(document.getElementById('s-hum-max').value);
  thresholds.gaz.max      = parseFloat(document.getElementById('s-gaz-max').value);
  thresholds.pression.min = parseFloat(document.getElementById('s-pres-min').value);
  thresholds.pression.max = parseFloat(document.getElementById('s-pres-max').value);

  document.getElementById('temp-range').textContent = `${thresholds.temp.min}°C – ${thresholds.temp.max}°C`;
  document.getElementById('hum-range').textContent  = `${thresholds.hum.min}% – ${thresholds.hum.max}%`;
  document.getElementById('gaz-range').textContent  = `${thresholds.gaz.max} ADC`;
  document.getElementById('pres-range').textContent = `${thresholds.pression.min} – ${thresholds.pression.max} hPa`;

  const ok = document.getElementById('settings-success');
  ok.classList.remove('hidden');
  setTimeout(() => ok.classList.add('hidden'), 2000);

  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({ type: 'update_thresholds', thresholds }));
  }
  addLog('Seuils mis à jour', 'ok');
});

// ============================================================
// EXPORT CSV
// ============================================================
document.getElementById('btn-export').addEventListener('click', () => {
  if (dataHistory.length === 0) { addLog('Aucune donnée à exporter', 'warn'); return; }
  const header = 'timestamp,temp,hum,pression,gaz,presence\n';
  const rows = dataHistory.map(d =>
    `${new Date().toISOString()},${d.temp},${d.hum},${d.pression},${d.gaz},${d.presence}`
  ).join('\n');
  const blob = new Blob([header + rows], { type: 'text/csv' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = `smart-cesi-${new Date().toISOString().split('T')[0]}.csv`;
  a.click();
  addLog('Export CSV téléchargé', 'ok');
});

// ============================================================
// LOGS
// ============================================================
function addLog(msg, type = '') {
  const container = document.getElementById('log-container');
  const entry = document.createElement('div');
  entry.className = 'log-entry ' + type;
  entry.textContent = `[${new Date().toLocaleTimeString('fr-FR')}] ${msg}`;
  container.prepend(entry);
  while (container.children.length > 50) container.removeChild(container.lastChild);
}

// ============================================================
// START
// ============================================================
connectWS();
addLog('Dashboard Smart CESI démarré', 'ok');
