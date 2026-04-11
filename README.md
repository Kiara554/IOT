# Smart CESI — Système de Monitoring IoT FabLab

Projet IoT — Surveillance environnementale en temps réel d'un FabLab via LoRa et WiFi, avec dashboard web.

---

## Présentation du projet

Ce système permet de surveiller en temps réel les conditions environnementales d'un FabLab :
- Température, humidité et pression atmosphérique (capteur BME280)
- Qualité de l'air (capteur MQ-135)
- Détection de présence (capteur PIR)

Les données sont transmises sans fil par **LoRa 868 MHz**, reçues par une seconde carte qui les envoie au dashboard via **WiFi**, et affichées sur une interface web en temps réel.

---

## Infrastructure

```
┌─────────────────────┐        LoRa 868 MHz        ┌──────────────────────┐
│   Heltec LoRa V3    │ ─────────────────────────► │   Heltec LoRa V3     │
│      ÉMETTEUR       │                            │      RÉCEPTEUR       │
│                     │                            │                      │
│  BME280 (T/H/P)     │                            │  Affichage OLED      │
│  MQ-135 (air)       │                            │  WiFi HTTP POST      │
│  PIR (présence)     │                            └──────────┬───────────┘
└─────────────────────┘                                       │
                                                               │ HTTP POST /api/data
                                                               ▼
                                                   ┌──────────────────────┐
                                                   │  PC — Node.js        │
                                                   │  server.js :3000     │
                                                   │                      │
                                                   │  WebSocket ──────►   │
                                                   │  Dashboard Web       │
                                                   └──────────────────────┘
```

---

## Matériel nécessaire

| Composant | Quantité | Rôle |
|---|---|---|
| Heltec WiFi LoRa 32 V3 | 2 | Émetteur + Récepteur |
| BME280 (I2C) | 1 | Température, Humidité, Pression |
| MQ-135 | 1 | Qualité de l'air |
| Capteur PIR HC-SR501 | 1 | Détection de présence |
| Câbles Dupont | — | Connexions |
| PC avec Node.js | 1 | Dashboard web |

---

## Branchements

### Émetteur — Heltec LoRa V3

#### BME280 (I2C)
| BME280 | Heltec V3 |
|---|---|
| VCC | 3.3V |
| GND | GND |
| SDA | GPIO 40 |
| SCL | GPIO 41 |

#### MQ-135 (analogique)
| MQ-135 | Heltec V3 |
|---|---|
| VCC | 5V |
| GND | GND |
| AO (sortie analogique) | GPIO 1 |

#### Capteur PIR HC-SR501
| PIR | Heltec V3 |
|---|---|
| VCC | 5V |
| GND | GND |
| OUT | GPIO 33 |

> L'écran OLED et la radio LoRa sont intégrés à la carte Heltec, aucun branchement supplémentaire n'est nécessaire.

---

## Structure du projet

```
projet/
├── code_emetteur/
│   └── code_emetteur.ino       # Code Arduino IDE — carte émettrice
├── code_recepteur/
│   ├── platformio.ini          # Config PlatformIO
│   └── src/
│       └── main.cpp            # Code récepteur avec WiFi (PlatformIO)
├── smart-cesi-dashboard/
│   ├── server.js               # Serveur Node.js + WebSocket
│   ├── package.json
│   └── public/
│       ├── index.html          # Interface web
│       ├── app.js              # Logique frontend
│       └── style.css           # Styles
└── README.md
```

---

## Installation et démarrage

### Prérequis
- [Arduino IDE](https://www.arduino.cc/en/software) avec la bibliothèque **Heltec ESP32 Dev-Boards**
- [PlatformIO](https://platformio.org/) (extension VSCode)
- [Node.js](https://nodejs.org/) v18+

---

### 1. Flasher l'émetteur (Arduino IDE)

1. Ouvre `code_emetteur/code_emetteur.ino` dans Arduino IDE
2. Sélectionne la carte : `Outils` → `Type de carte` → `Heltec WiFi LoRa 32(V3)`
3. Sélectionne le port COM de la carte émettrice
4. Clique sur **Téléverser**

---

### 2. Configurer et flasher le récepteur (PlatformIO)

1. Ouvre le dossier `code_recepteur/` dans VSCode
2. Modifie les lignes suivantes dans `src/main.cpp` :

```cpp
const char* WIFI_SSID     = "TON_SSID";           // Nom de ton réseau WiFi
const char* WIFI_PASSWORD = "TON_MOT_DE_PASSE";   // Mot de passe WiFi
const char* SERVER_URL    = "http://192.168.1.XX:3000/api/data"; // IP de ton PC
```

Pour trouver l'IP de ton PC : lance `ipconfig` dans le terminal et cherche **"Carte réseau sans fil Wi-Fi" → "Adresse IPv4"**.

3. Dans le terminal VSCode :
```bash
cd code_recepteur
pio run --target upload
```

---

### 3. Démarrer le dashboard

```bash
cd smart-cesi-dashboard
npm install
node server.js
```

Ouvre ensuite `http://localhost:3000` dans ton navigateur.

---

## Format des trames LoRa

L'émetteur envoie une trame texte toutes les 5 secondes :

```
T:21.5,H:48.2,P:1016,G:355,Presence:1
```

| Champ | Valeur | Unité |
|---|---|---|
| T | Température | °C |
| H | Humidité relative | % |
| P | Pression atmosphérique | hPa |
| G | Qualité de l'air (ADC) | 0–4095 |
| Presence | Détection PIR | 0 ou 1 |

Le récepteur envoie cette trame au serveur via HTTP POST en JSON :

```json
{
  "payload": "T:21.5,H:48.2,P:1016,G:355,Presence:1",
  "rssi": -43
}
```

---

## Fonctionnalités du dashboard

- Affichage temps réel des 5 capteurs
- Graphiques avec les 60 dernières mesures
- Alertes configurables par seuil (température, humidité, pression, qualité air)
- Indicateur de présence avec horodatage
- Export CSV des données
- Logs temps réel
- Sauvegarde automatique en CSV côté serveur (`logs/data_YYYY-MM-DD.csv`)

---

## Paramètres LoRa

| Paramètre | Valeur |
|---|---|
| Fréquence | 868 MHz |
| Spreading Factor | 7 |
| Bandwidth | 125 kHz |
| Coding Rate | 4/5 |
| Puissance TX | 14 dBm |

> Les deux cartes doivent avoir exactement les mêmes paramètres LoRa pour communiquer.
