#include <Arduino.h>
#include <RadioLib.h>
#include <Wire.h>
#include <SSD1306Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "mbedtls/aes.h"
#include "../config.h"

// ============================================================
// PINS Heltec WiFi LoRa 32 V3 — SX1262
// ============================================================
#define LORA_NSS   8
#define LORA_DIO1  14
#define LORA_RST   12
#define LORA_BUSY  13

// ============================================================
// PINS OLED Heltec V3
// ============================================================
#define OLED_SDA  17
#define OLED_SCL  18
#define OLED_RST  21
#define Vext      36

// ============================================================
// LORA — mêmes paramètres que l'émetteur
// ============================================================
#define LORA_FREQ       868.0
#define LORA_BW         125.0
#define LORA_SF         7
#define LORA_CR         5      // coding rate 4/5
#define LORA_SYNC       0x12
#define LORA_POWER      14

SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);
SSD1306Wire display(0x3c, OLED_SDA, OLED_SCL);

volatile bool receivedFlag = false;

void IRAM_ATTR setFlag() {
  receivedFlag = true;
}

// ============================================================
// AES-128-CBC — déchiffrement
// Clé et IV partagés avec l'émetteur (définis dans config.h)
// ============================================================
static const uint8_t aes_key[16] = AES_KEY_BYTES;
static const uint8_t aes_iv[16]  = AES_IV_BYTES;

static uint8_t hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

// Reçoit une String de caractères hex (ex: "a3f5b2c1...")
// Retourne le texte clair dans `plain`, ou false si échec.
bool decryptAES128(const String& hexCipher, String& plain) {
  const size_t hexLen = hexCipher.length();
  // Doit être un multiple de 32 (= blocs de 16 octets encodés en hex)
  if (hexLen == 0 || hexLen % 32 != 0) return false;

  // Vérification rapide : tous les caractères doivent être hex
  for (size_t i = 0; i < hexLen; i++) {
    char c = hexCipher[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
      return false;
  }

  const size_t cipherLen = hexLen / 2;
  uint8_t* cipher = (uint8_t*)malloc(cipherLen);
  uint8_t* output = (uint8_t*)malloc(cipherLen + 1);
  if (!cipher || !output) { free(cipher); free(output); return false; }

  // Décodage hex → octets
  for (size_t i = 0; i < cipherLen; i++) {
    cipher[i] = (hexNibble(hexCipher[i * 2]) << 4) | hexNibble(hexCipher[i * 2 + 1]);
  }

  // Déchiffrement AES-128-CBC
  uint8_t iv[16];
  memcpy(iv, aes_iv, 16);  // l'IV est modifié lors du déchiffrement, on travaille sur une copie

  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_dec(&ctx, aes_key, 128);
  int ret = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, cipherLen, iv, cipher, output);
  mbedtls_aes_free(&ctx);
  free(cipher);

  if (ret != 0) { free(output); return false; }

  // Suppression du padding PKCS7
  const uint8_t padVal = output[cipherLen - 1];
  if (padVal == 0 || padVal > 16) { free(output); return false; }
  const size_t plainLen = cipherLen - padVal;
  output[plainLen] = '\0';

  plain = String((char*)output);
  free(output);
  return true;
}

// ============================================================
// BUFFER CIRCULAIRE — stockage local si WiFi indisponible
// ============================================================
#define FRAME_BUF_SIZE 20

struct FrameEntry {
  String payload;
  int    rssi;
};

static FrameEntry frameBuf[FRAME_BUF_SIZE];
static int  bufHead  = 0;   // indice de la trame la plus ancienne
static int  bufTail  = 0;   // prochain emplacement d'écriture
static int  bufCount = 0;   // nombre de trames stockées

static void bufferFrame(const String& payload, int rssi) {
  frameBuf[bufTail] = {payload, rssi};
  bufTail = (bufTail + 1) % FRAME_BUF_SIZE;
  if (bufCount < FRAME_BUF_SIZE) {
    bufCount++;
  } else {
    // Buffer plein : on écrase la plus ancienne trame
    bufHead = (bufHead + 1) % FRAME_BUF_SIZE;
  }
}

static bool dequeueFrame(String& payload, int& rssi) {
  if (bufCount == 0) return false;
  payload = frameBuf[bufHead].payload;
  rssi    = frameBuf[bufHead].rssi;
  bufHead = (bufHead + 1) % FRAME_BUF_SIZE;
  bufCount--;
  return true;
}

// ============================================================
// NUMÉRO DE SÉQUENCE — extrait du champ S: du plaintext déchiffré
// ============================================================
// Extrait la valeur de S: dans "S:42,T:21.5,..."
// Retourne -1 si absent.
static long extractSeq(const String& trame) {
  int idx = trame.indexOf("S:");
  if (idx < 0) return -1;
  return trame.substring(idx + 2).toInt();
}

// ============================================================
// WiFi — essaie les réseaux définis dans config.h dans l'ordre
// ============================================================
struct WifiNetwork { const char* ssid; const char* password; const char* serverUrl; };

static const WifiNetwork WIFI_NETWORKS[] = {
  { WIFI_SSID_1, WIFI_PASSWORD_1, SERVER_URL_1 },
  { WIFI_SSID_2, WIFI_PASSWORD_2, SERVER_URL_2 },
};
static const int WIFI_NETWORK_COUNT = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);

// URL active — mise à jour selon le réseau qui se connecte
static const char* activeServerUrl = SERVER_URL_1;

void connectWiFi() {
  for (int n = 0; n < WIFI_NETWORK_COUNT; n++) {
    const WifiNetwork& net = WIFI_NETWORKS[n];
    Serial.printf("Connexion WiFi à %s...\n", net.ssid);
    WiFi.begin(net.ssid, net.password);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      activeServerUrl = net.serverUrl;
      Serial.printf("\nWiFi connecté — réseau: %s | IP: %s | serveur: %s\n",
                    net.ssid, WiFi.localIP().toString().c_str(), activeServerUrl);
      return;
    }
    Serial.printf("\nEchec sur %s\n", net.ssid);
    WiFi.disconnect();
    delay(200);
  }
  Serial.println("Aucun réseau disponible");
}

// Supprime les caractères de contrôle et les guillemets/backslash du payload
// pour éviter de casser le JSON construit manuellement.
static String sanitizePayload(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c >= 0x20 && c != '"' && c != '\\') out += c;
  }
  return out;
}

// Vérifie que le texte déchiffré ressemble à une trame valide
// Le format attendu est "S:xx,T:xx.x,..." ou "T:xx.x,..."
static bool isValidTrame(const String& s) {
  return s.length() > 5 && (s.startsWith("S:") || s.startsWith("T:"));
}

// Effectue le POST HTTP. Retourne true si succès (2xx).
static bool httpPost(const String& payload, int rssi) {
  const String safe = sanitizePayload(payload);

  if (!isValidTrame(safe)) {
    Serial.println("Trame ignorée après sanitize (contenu invalide)");
    return false;
  }

  // Extrait le seqNum de l'émetteur depuis le champ S: du plaintext
  const long seq = extractSeq(safe);

  HTTPClient http;
  http.begin(activeServerUrl);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Auth-Token", AUTH_TOKEN);

  String body = "{\"payload\":\"" + safe + "\",\"rssi\":" + rssi;
  if (seq >= 0) body += ",\"seq\":" + String(seq);
  body += "}";

  int code = http.POST(body);
  http.end();

  if (code > 0 && code < 300) {
    Serial.printf("HTTP POST OK: %d\n", code);
    return true;
  }
  Serial.printf("HTTP POST erreur: %s (code %d)\n",
                http.errorToString(code).c_str(), code);
  return false;
}

// Vide le buffer en envoyant les trames en attente.
static void flushBuffer() {
  if (bufCount == 0) return;
  Serial.printf("WiFi rétabli — vidage de %d trame(s) en attente\n", bufCount);
  String p; int r;
  while (bufCount > 0 && WiFi.status() == WL_CONNECTED) {
    if (!dequeueFrame(p, r)) break;
    if (!httpPost(p, r)) {
      // En cas d'échec pendant le vidage, on rebufferise et on arrête
      bufferFrame(p, r);
      Serial.println("Echec vidage — arrêt, trames restantes conservées");
      break;
    }
  }
}

void sendToServer(const String& payload, int rssi) {
  // Tentative de reconnexion si nécessaire
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (WiFi.status() != WL_CONNECTED) {
    // Toujours hors ligne : on bufferise
    bufferFrame(payload, rssi);
    Serial.printf("WiFi KO — trame bufferisée (%d/%d)\n", bufCount, FRAME_BUF_SIZE);
    return;
  }

  // WiFi dispo : vider le buffer d'abord
  if (bufCount > 0) flushBuffer();

  // Envoyer la trame courante
  if (!httpPost(payload, rssi)) {
    bufferFrame(payload, rssi);
    Serial.printf("POST échoué — trame bufferisée (%d/%d)\n", bufCount, FRAME_BUF_SIZE);
  }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);

  // Alimentation OLED
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
  delay(100);

  // OLED
  Wire.begin(OLED_SDA, OLED_SCL);
  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, LOW);
  delay(50);
  digitalWrite(OLED_RST, HIGH);

  display.init();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);
  display.clear();
  display.drawString(0, 0, "Connexion WiFi...");
  display.display();

  // WiFi
  connectWiFi();

  // OLED — état WiFi
  display.clear();
  if (WiFi.status() == WL_CONNECTED) {
    display.drawString(0, 0, "WiFi OK");
    display.drawString(0, 15, WiFi.localIP().toString());
  } else {
    display.drawString(0, 0, "WiFi ECHEC");
  }
  display.drawString(0, 35, "En attente LoRa...");
  display.display();

  // LoRa SX1262
  Serial.print("Initialisation LoRa...");
  int state = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR, LORA_SYNC, LORA_POWER);
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println(" OK");
  } else {
    Serial.printf(" ERREUR: %d\n", state);
  }

  radio.setDio1Action(setFlag);
  radio.startReceive();
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  if (!receivedFlag) return;
  receivedFlag = false;

  String rxData;
  int state = radio.readData(rxData);

  if (state == RADIOLIB_ERR_NONE) {
    int rssi = (int)radio.getRSSI();
    Serial.printf("RECU (chiffré): %s | RSSI: %d dBm\n", rxData.c_str(), rssi);

    // Déchiffrement AES-128-CBC
    String plaintext;
    if (!decryptAES128(rxData, plaintext)) {
      Serial.println("ERREUR: déchiffrement AES échoué — trame ignorée");
      display.clear();
      display.drawString(0, 0, "Erreur AES");
      display.drawString(0, 20, rxData.substring(0, 20));
      display.display();
      radio.startReceive();
      return;
    }
    Serial.printf("DECHIFFRE: %s\n", plaintext.c_str());

    // OLED — mise à jour avant traitement long
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.drawString(0, 0, "Donnees Recues:");
    display.drawString(0, 20, plaintext.substring(0, 24));
    display.drawString(0, 45, "Signal: " + String(rssi) + " dBm");
    display.display();

    // Remettre LoRa en écoute AVANT le HTTP POST (bloquant)
    radio.startReceive();

    // Envoi WiFi
    sendToServer(plaintext, rssi);

  } else {
    Serial.printf("Erreur reception: %d\n", state);
    radio.startReceive();
  }
}
