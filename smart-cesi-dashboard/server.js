require('dotenv').config();
const ngrok      = require('@ngrok/ngrok');
const express    = require('express');
const session    = require('express-session');
const WebSocket  = require('ws');
const mqtt       = require('mqtt');
const path       = require('path');
const fs         = require('fs');
const crypto     = require('crypto');

// ============================================================
// CONFIGURATION
// ============================================================
const CONFIG = {
  webPort:     3000,
  historySize: 60
};

const DASHBOARD_PASSWORD = process.env.DASHBOARD_PASSWORD || 'smartcesi2026';

// Seuils d'alerte
const ALERT_TEMP_MAX = 26;   // °C
const ALERT_GAZ_MAX  = 800;  // ppm

// ============================================================
// REGISTRE CHAINÉ SHA-256 (blockchain légère)
// Chaque bloc : { index, timestamp, data_hash, previous_hash, block_hash }
// ============================================================
let chain = [];
let lastBlockHash = '0'.repeat(64); // hash du bloc genesis

function addBlock(data) {
  const index     = chain.length;
  const timestamp = new Date().toISOString();
  const dataHash  = crypto.createHash('sha256')
                          .update(JSON.stringify(data) + timestamp)
                          .digest('hex');
  const blockRaw  = `${index}${timestamp}${dataHash}${lastBlockHash}`;
  const blockHash = crypto.createHash('sha256').update(blockRaw).digest('hex');

  const block = {
    index,
    timestamp,
    data_hash:     dataHash,
    previous_hash: lastBlockHash,
    block_hash:    blockHash,
    hmac_valid:    data.hmac_valid ?? null
  };
  chain.push(block);
  lastBlockHash = blockHash;
  return block;
}

// ============================================================
// NUMÉRO DE SÉQUENCE — détection de sauts
// ============================================================
let lastSeqNum = -1;

function checkSeq(seq) {
  if (typeof seq !== 'number') return;
  if (lastSeqNum >= 0 && seq !== lastSeqNum + 1) {
    const lost = seq - (lastSeqNum + 1);
    console.warn(`[SEQ] Saut de séquence — attendu ${lastSeqNum + 1}, reçu ${seq} (${lost} trame(s) perdue(s))`);
    broadcast({ type: 'seq_loss', lost, expected: lastSeqNum + 1, received: seq });
    stats.totalErrors += 1;
  }
  lastSeqNum = seq;
}

// ============================================================
// LOGS CSV
// ============================================================
const LOGS_DIR = path.join(__dirname, 'logs');
if (!fs.existsSync(LOGS_DIR)) fs.mkdirSync(LOGS_DIR);

let currentLogFile = null;
let currentLogDate = null;

function initLog() {
  const today = new Date().toISOString().split('T')[0];
  if (currentLogDate !== today) {
    currentLogDate = today;
    currentLogFile = path.join(LOGS_DIR, `data_${today}.csv`);
    if (!fs.existsSync(currentLogFile)) {
      fs.writeFileSync(currentLogFile, 'timestamp,temp,hum,pression,gaz,presence,seq\n');
    }
  }
}

function logCSV(data) {
  try {
    initLog();
    const line = `${new Date().toISOString()},${data.temp||''},${data.hum||''},${data.pression||''},${data.gaz||''},${data.presence||''},${data.seq ?? ''}\n`;
    fs.appendFileSync(currentLogFile, line);
  } catch(e) {
    console.error('Erreur CSV:', e.message);
  }
}

// ============================================================
// PARSING — convertit la trame LoRa en objet JSON
// "T:21.5,H:48.2,P:1016,G:355,Presence:1"
// ============================================================
function parseTrame(line) {
  try {
    line = line.trim();

    // Si c'est déjà du JSON
    if (line.startsWith('{')) return JSON.parse(line);

    // Format T:xx,H:xx,P:xx,G:xx,Presence:x
    const obj = {};
    line.split(',').forEach(part => {
      const [key, val] = part.split(':');
      if (!key || val === undefined) return;
      const k = key.trim().toUpperCase();
      const v = parseFloat(val.trim());
      if      (k === 'S')        obj.seq      = v;
      else if (k === 'T')        obj.temp     = v;
      else if (k === 'H')        obj.hum      = v;
      else if (k === 'P')        obj.pression = v;
      else if (k === 'G')        obj.gaz      = v;
      else if (k === 'PRESENCE') obj.presence = v;
    });

    if (obj.temp === undefined) return null;
    return obj;

  } catch(e) {
    return null;
  }
}

// ============================================================
// TRAITEMENT COMMUN — utilisé par MQTT et le fallback HTTP
// body doit contenir : payload, rssi?, hmac_valid?, seq?
// Retourne { ok, error?, block_index? }
// ============================================================
function processIncomingData(body) {
  if (!body || !body.payload) {
    stats.totalErrors++;
    return { ok: false, error: 'payload manquant' };
  }

  const data = parseTrame(body.payload);
  if (!data) {
    stats.totalErrors++;
    console.error('Trame invalide:', body.payload);
    return { ok: false, error: 'trame invalide' };
  }

  data.timestamp  = Date.now();
  if (body.rssi       !== undefined) data.rssi       = body.rssi;
  if (body.hmac_valid !== undefined) data.hmac_valid = body.hmac_valid;

  const seqVal = data.seq ?? (body.seq !== undefined ? Number(body.seq) : undefined);
  if (seqVal !== undefined) {
    checkSeq(seqVal);
    data.seq = seqVal;
  }

  const block = addBlock(data);
  console.log(`[CHAIN] Bloc #${block.index} — hash: ${block.block_hash.slice(0, 16)}...`);

  stats.totalReceived++;
  stats.lastUpdate = new Date().toISOString();
  isConnected  = true;
  lastDataTime = Date.now();

  dataHistory.push(data);
  if (dataHistory.length > CONFIG.historySize) dataHistory.shift();

  logCSV(data);
  broadcast({ type: 'data', payload: data });
  broadcast({ type: 'status', connected: true, port: 'MQTT', stats });

  console.log(`[#${data.seq ?? '?'}] T:${data.temp}°C H:${data.hum}% P:${data.pression}hPa G:${data.gaz} Pres:${data.presence} RSSI:${data.rssi||'?'}dBm`);

  // Publication de l'état courant avec Retain = true
  const statePayload = JSON.stringify({ temp: data.temp, hum: data.hum, pression: data.pression,
                                        gaz: data.gaz, presence: data.presence, seq: data.seq,
                                        rssi: data.rssi, timestamp: data.timestamp });
  mqttClient.publish('campus/fablab/zone1/env/state', statePayload, { qos: 1, retain: true });

  // Publication d'une alerte si seuils dépassés — Retain = false
  const alerts = [];
  if (data.temp  !== undefined && data.temp  > ALERT_TEMP_MAX) alerts.push(`temp=${data.temp}`);
  if (data.gaz   !== undefined && data.gaz   > ALERT_GAZ_MAX)  alerts.push(`gaz=${data.gaz}`);
  if (alerts.length > 0) {
    const alertPayload = JSON.stringify({ alert: true, values: alerts, timestamp: data.timestamp });
    mqttClient.publish('campus/fablab/zone1/env/alert', alertPayload, { qos: 1, retain: false });
    console.warn(`[MQTT] Alerte publiée : ${alerts.join(', ')}`);
  }

  return { ok: true, block_index: block.index };
}

// ============================================================
// EXPRESS + WEBSOCKET
// ============================================================
const app    = express();
const server = require('http').createServer(app);
const wss    = new WebSocket.Server({ server });

// Session
const sessionMiddleware = session({
  secret: process.env.SESSION_SECRET || 'smartcesi-session-secret',
  resave: false,
  saveUninitialized: false,
  cookie: { maxAge: 8 * 60 * 60 * 1000 } // 8h
});
app.use(sessionMiddleware);

app.use(express.json());
app.use(express.urlencoded({ extended: false }));

// ============================================================
// AUTHENTIFICATION DASHBOARD
// ============================================================
function requireAuth(req, res, next) {
  if (req.session && req.session.authenticated) return next();
  res.redirect('/login');
}

// Routes publiques (pas de requireAuth)
app.get('/login', (_req, res) => res.sendFile(path.join(__dirname, 'public', 'login.html')));

app.post('/login', (req, res) => {
  if (req.body.password === DASHBOARD_PASSWORD) {
    req.session.authenticated = true;
    res.redirect('/');
  } else {
    res.redirect('/login?error=1');
  }
});

app.get('/logout', (req, res) => {
  req.session.destroy(() => res.redirect('/login'));
});

// Fichiers statiques protégés par session
app.use(requireAuth, express.static(path.join(__dirname, 'public')));

const clients = new Set();
let dataHistory = [];
let stats = { totalReceived: 0, totalErrors: 0, lastUpdate: null };
let isConnected = false;
let lastDataTime = null;

// Vérifie toutes les 5s si la carte est toujours active (timeout 15s)
setInterval(() => {
  if (lastDataTime && isConnected) {
    const secondsSinceLastData = (Date.now() - lastDataTime) / 1000;
    if (secondsSinceLastData > 15) {
      isConnected = false;
      broadcast({ type: 'status', connected: false, port: 'MQTT', stats });
      console.log('Carte déconnectée — aucune donnée depuis', Math.round(secondsSinceLastData), 'secondes');
    }
  }
}, 5000);

wss.on('connection', (ws, req) => {
  const fakeRes = { getHeader: () => {}, setHeader: () => {}, on: () => {}, end: () => {} };

  sessionMiddleware(req, fakeRes, () => {
    if (!req.session || !req.session.authenticated) {
      ws.close(1008, 'Unauthorized');
      return;
    }

    clients.add(ws);
    console.log('Client WebSocket connecté');

    ws.send(JSON.stringify({ type: 'status', connected: isConnected, port: 'MQTT', stats }));
    ws.send(JSON.stringify({ type: 'history', data: dataHistory }));

    ws.on('message', msg => {
      try {
        const data = JSON.parse(msg);
        if (data.type === 'update_thresholds') {
          console.log('Seuils mis à jour:', data.thresholds);
        }
      } catch(e) {}
    });

    ws.on('close', () => clients.delete(ws));
    ws.on('error', () => clients.delete(ws));
  });
});

function broadcast(obj) {
  const msg = JSON.stringify(obj);
  for (const c of clients) {
    if (c.readyState === WebSocket.OPEN) c.send(msg);
  }
}

// ============================================================
// ROUTES
// ============================================================
app.get('/api/status',  requireAuth, (_req, res) => res.json({ connected: isConnected, stats }));
app.get('/api/history', requireAuth, (_req, res) => res.json(dataHistory));
app.get('/api/chain',   requireAuth, (_req, res) => res.json(chain));

// ============================================================
// CLIENT MQTT — connexion au broker Mosquitto local
// ============================================================
const mqttClient = mqtt.connect('mqtt://localhost:1883', {
  username:  'fablab',
  password:  'SmartCESI2026',
  clientId:  'smart-cesi-server',
  clean:     true,
  reconnectPeriod: 5000
});

mqttClient.on('connect', () => {
  const topics = [
    'campus/fablab/zone1/env/data',
    'campus/fablab/zone1/env/alert',
    'campus/fablab/zone1/env/state',
    'campus/fablab/zone1/presence'
  ];
  topics.forEach(topic => mqttClient.subscribe(topic, { qos: 1 }));
  console.log('[MQTT] Connecté au broker Mosquitto — topics souscrits');
});

mqttClient.on('message', (topic, message) => {
  let body;
  try {
    body = JSON.parse(message.toString());
  } catch(e) {
    console.error(`[MQTT] Message non-JSON sur ${topic}:`, message.toString());
    return;
  }

  if (topic === 'campus/fablab/zone1/env/data') {
    processIncomingData(body);

  } else if (topic === 'campus/fablab/zone1/presence') {
    console.log('[MQTT] Présence détectée:', body);
    broadcast({ type: 'presence', payload: body });

  } else if (topic === 'campus/fablab/zone1/env/alert') {
    console.warn('[MQTT] Alerte reçue:', body);
    broadcast({ type: 'alert', payload: body });

  } else if (topic === 'campus/fablab/zone1/env/state') {
    // État retenu par le broker — pas de retraitement, simple diffusion si clients connectés
    broadcast({ type: 'retained_state', payload: body });
  }
});

mqttClient.on('error', err => {
  console.error('[MQTT] Erreur:', err.message);
});

mqttClient.on('offline', () => {
  console.warn('[MQTT] Connexion au broker perdue — tentative de reconnexion...');
});

// ============================================================
// NGROK — tunnel public optionnel
// ============================================================
server.listen(CONFIG.webPort, async () => {
  console.log('\n╔════════════════════════════════════╗');
  console.log('║     Smart CESI — FabLab Monitor    ║');
  console.log('╚════════════════════════════════════╝\n');
  console.log(`Dashboard : http://localhost:${CONFIG.webPort}`);
  console.log(`Réception : MQTT broker localhost:1883  [auth: fablab / Mosquitto]`);
  console.log(`Fallback  : HTTP POST → /api/data  (sans X-Auth-Token)`);
  console.log(`Registre  : GET /api/chain`);
  console.log(`Logs CSV  : ${LOGS_DIR}\n`);

  if (process.env.NGROK_TOKEN) {
    try {
      const listener = await ngrok.forward({ addr: CONFIG.webPort, authtoken: process.env.NGROK_TOKEN });
      console.log(`[NGROK] Tunnel actif → ${listener.url()}`);
    } catch(e) {
      console.warn(`[NGROK] Échec : ${e.message}`);
    }
  } else {
    console.log('[NGROK] Non configuré — ajouter NGROK_TOKEN dans .env pour activer le tunnel');
  }
});
