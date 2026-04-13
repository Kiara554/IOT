require('dotenv').config();
const ngrok      = require('@ngrok/ngrok');
const express    = require('express');
const session    = require('express-session');
const WebSocket  = require('ws');
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

// Token partagé avec le récepteur LoRa (doit correspondre à AUTH_TOKEN dans config.h)
const AUTH_TOKEN        = process.env.AUTH_TOKEN        || 'SmartCESI-Secret-2026';
const DASHBOARD_PASSWORD = process.env.DASHBOARD_PASSWORD || 'smartcesi2026';

// ============================================================
// MIDDLEWARE — vérification du token sur POST /api/data
// ============================================================
function checkAuthToken(req, res, next) {
  const token = req.headers['x-auth-token'];
  if (!token || token !== AUTH_TOKEN) {
    console.warn(`[AUTH] Requête rejetée — X-Auth-Token invalide ou absent (IP: ${req.ip})`);
    return res.status(401).json({ error: 'Unauthorized' });
  }
  next();
}

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
    stats.totalErrors += 1;  // 1 événement de perte, quel que soit le nombre de trames perdues
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

// Route ESP32 — avant les fichiers statiques pour ne pas être interceptée par requireAuth
app.post('/api/data', checkAuthToken, (req, res) => {
  const body = req.body;

  if (!body || !body.payload) {
    stats.totalErrors++;
    return res.status(400).json({ error: 'payload manquant' });
  }

  const data = parseTrame(body.payload);
  if (!data) {
    stats.totalErrors++;
    console.error('Trame invalide:', body.payload);
    return res.status(422).json({ error: 'trame invalide' });
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
  isConnected = true;
  lastDataTime = Date.now();

  dataHistory.push(data);
  if (dataHistory.length > CONFIG.historySize) dataHistory.shift();

  logCSV(data);
  broadcast({ type: 'data', payload: data });
  broadcast({ type: 'status', connected: true, port: 'WiFi', stats });

  console.log(`[#${data.seq ?? '?'}] T:${data.temp}°C H:${data.hum}% P:${data.pression}hPa G:${data.gaz} Pres:${data.presence} RSSI:${data.rssi||'?'}dBm`);
  res.json({ ok: true, block_index: block.index });
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
      broadcast({ type: 'status', connected: false, port: 'WiFi', stats });
      console.log('Carte déconnectée — aucune donnée depuis', Math.round(secondsSinceLastData), 'secondes');
    }
  }
}, 5000);

wss.on('connection', (ws, req) => {
  // express-session a besoin d'un objet réponse minimal pour ne pas planter
  const fakeRes = { getHeader: () => {}, setHeader: () => {}, on: () => {}, end: () => {} };

  sessionMiddleware(req, fakeRes, () => {
    if (!req.session || !req.session.authenticated) {
      ws.close(1008, 'Unauthorized');
      return;
    }

    clients.add(ws);
    console.log('Client WebSocket connecté');

    ws.send(JSON.stringify({ type: 'status', connected: isConnected, port: 'WiFi', stats }));
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
// NGROK — tunnel public optionnel
// Activer : définir la variable d'environnement NGROK_AUTHTOKEN
//   Windows : set NGROK_AUTHTOKEN=ton_token && npm start
//   ou créer un fichier .env avec NGROK_AUTHTOKEN=ton_token
// Obtenir un token gratuit sur https://dashboard.ngrok.com
// ============================================================
// ============================================================
// START
// ============================================================
server.listen(CONFIG.webPort, async () => {
  console.log('\n╔════════════════════════════════════╗');
  console.log('║     Smart CESI — FabLab Monitor    ║');
  console.log('╚════════════════════════════════════╝\n');
  console.log(`Dashboard : http://localhost:${CONFIG.webPort}`);
  console.log(`Réception : WiFi HTTP POST → /api/data  [auth: X-Auth-Token]`);
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
