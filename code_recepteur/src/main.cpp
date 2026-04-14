#include <Arduino.h>
#include <RadioLib.h>
#include <Wire.h>
#include <SSD1306Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
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

WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

volatile bool receivedFlag = false;

void IRAM_ATTR setFlag() {
  receivedFlag = true;
}

// ============================================================
// AES-128-CBC — déchiffrement
// Clé et IV partagés avec l'émetteur (définis dans config.h)
// ============================================================
static const uint8_t aes_key[16]  = AES_KEY_BYTES;
static const uint8_t aes_iv[16]   = AES_IV_BYTES;
static const uint8_t hmac_key[16] = HMAC_KEY_BYTES;

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
// HMAC-SHA256 — vérification
// Reçoit la trame déchiffrée complète (avec ",MAC:xxxx" à la fin).
// Retourne true si le HMAC est valide, et place le payload sans MAC dans `data`.
// ============================================================
bool verifyAndStripHMAC(const String& full, String& data, bool& hmacValid) {
  const int macIdx = full.indexOf(",MAC:");
  if (macIdx < 0) {
    // Pas de champ MAC — trame ancienne ou sans HMAC
    data      = full;
    hmacValid = false;
    return true;  // on laisse passer mais on signale
  }

  data             = full.substring(0, macIdx);
  String rxMacHex  = full.substring(macIdx + 5, macIdx + 21); // 16 hex chars

  // Recalcul du HMAC sur la partie données
  uint8_t hmac[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, hmac_key, sizeof(hmac_key));
  mbedtls_md_hmac_update(&ctx, (const uint8_t*)data.c_str(), data.length());
  mbedtls_md_hmac_finish(&ctx, hmac);
  mbedtls_md_free(&ctx);

  char expected[17];
  for (int i = 0; i < 8; i++) sprintf(expected + i * 2, "%02x", hmac[i]);
  expected[16] = '\0';

  hmacValid = (rxMacHex == String(expected));
  if (!hmacValid) {
    Serial.printf("HMAC invalide — attendu %s, reçu %s\n", expected, rxMacHex.c_str());
  }
  return true;
}

// ============================================================
// BUFFER CIRCULAIRE — stockage local si broker MQTT injoignable
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
static long extractSeq(const String& trame) {
  int idx = trame.indexOf("S:");
  if (idx < 0) return -1;
  return trame.substring(idx + 2).toInt();
}

// ============================================================
// WiFi — essaie les réseaux définis dans config.h dans l'ordre
// ============================================================
struct WifiNetwork { const char* ssid; const char* password; };

static const WifiNetwork WIFI_NETWORKS[] = {
  { WIFI_SSID_1, WIFI_PASSWORD_1 },
  { WIFI_SSID_2, WIFI_PASSWORD_2 },
};
static const int WIFI_NETWORK_COUNT = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);

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
      Serial.printf("\nWiFi connecté — réseau: %s | IP: %s\n",
                    net.ssid, WiFi.localIP().toString().c_str());
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
static bool isValidTrame(const String& s) {
  return s.length() > 5 && (s.startsWith("S:") || s.startsWith("T:"));
}

// ============================================================
// MQTT — connexion / reconnexion
// Clean Session = false → session persistante côté broker
// ============================================================
static bool mqttReconnect() {
  if (mqttClient.connected()) return true;

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);

  Serial.printf("[MQTT] Connexion au broker %s:%d...\n", MQTT_BROKER, MQTT_PORT);
  // connect(clientId, user, pass, willTopic, willQoS, willRetain, willMessage, cleanSession=false)
  bool ok = mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD,
                               nullptr, 0, false, nullptr, false);
  if (ok) {
    Serial.println("[MQTT] Connecté au broker");
  } else {
    Serial.printf("[MQTT] Echec connexion — état: %d\n", mqttClient.state());
  }
  return ok;
}

// ============================================================
// MQTT — publication d'une trame
// Publie sur campus/fablab/zone1/env/data
// Publie sur campus/fablab/zone1/presence si Presence:1 détecté
// ============================================================
static bool mqttPublish(const String& payload, int rssi) {
  // 1. Vérification et retrait du HMAC
  String data;
  bool   hmacValid = false;
  verifyAndStripHMAC(payload, data, hmacValid);

  const String safe = sanitizePayload(data);

  if (!isValidTrame(safe)) {
    Serial.println("Trame ignorée après sanitize (contenu invalide)");
    return false;
  }

  // 2. Extraction du numéro de séquence
  const long seq = extractSeq(safe);

  // 3. Construction du JSON
  String jsonPayload = "{\"payload\":\"" + safe + "\",\"rssi\":" + rssi
                     + ",\"hmac_valid\":" + (hmacValid ? "true" : "false");
  if (seq >= 0) jsonPayload += ",\"seq\":" + String(seq);
  jsonPayload += "}";

  // 4. Connexion broker si nécessaire
  if (!mqttClient.connected()) {
    if (!mqttReconnect()) return false;
  }

  // 5. Publication — QoS 0 (limite de la bibliothèque PubSubClient pour les publishes)
  bool ok = mqttClient.publish("campus/fablab/zone1/env/data", jsonPayload.c_str());

  // 6. Publication séparée présence si détectée
  if (safe.indexOf("Presence:1") >= 0) {
    mqttClient.publish("campus/fablab/zone1/presence", "{\"presence\":1}");
    Serial.println("[MQTT] Présence détectée — publié sur campus/fablab/zone1/presence");
  }

  if (ok) {
    Serial.printf("[MQTT] Publié sur campus/fablab/zone1/env/data : %s\n", jsonPayload.c_str());
  } else {
    Serial.println("[MQTT] Echec publication");
  }
  return ok;
}

// Vide le buffer en republiant les trames en attente.
static void flushBuffer() {
  if (bufCount == 0) return;
  Serial.printf("[MQTT] Broker rétabli — vidage de %d trame(s) en attente\n", bufCount);
  String p; int r;
  while (bufCount > 0 && mqttClient.connected()) {
    if (!dequeueFrame(p, r)) break;
    if (!mqttPublish(p, r)) {
      // En cas d'échec pendant le vidage, on rebufferise et on arrête
      bufferFrame(p, r);
      Serial.println("Echec vidage — arrêt, trames restantes conservées");
      break;
    }
  }
}

void sendToServer(const String& payload, int rssi) {
  // 1. Assurer la connexion WiFi
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  if (WiFi.status() != WL_CONNECTED) {
    bufferFrame(payload, rssi);
    Serial.printf("WiFi KO — trame bufferisée (%d/%d)\n", bufCount, FRAME_BUF_SIZE);
    return;
  }

  // 2. Assurer la connexion MQTT
  if (!mqttClient.connected()) {
    if (!mqttReconnect()) {
      bufferFrame(payload, rssi);
      Serial.printf("[MQTT] Broker KO — trame bufferisée (%d/%d)\n", bufCount, FRAME_BUF_SIZE);
      return;
    }
  }

  // 3. Vider le buffer d'abord
  if (bufCount > 0) flushBuffer();

  // 4. Envoyer la trame courante
  if (!mqttPublish(payload, rssi)) {
    bufferFrame(payload, rssi);
    Serial.printf("[MQTT] Publication échouée — trame bufferisée (%d/%d)\n", bufCount, FRAME_BUF_SIZE);
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

  // MQTT — connexion initiale
  mqttReconnect();

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
  // Maintien de la connexion MQTT
  if (WiFi.status() == WL_CONNECTED) {
    mqttClient.loop();
  }

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

    // Remettre LoRa en écoute AVANT la publication MQTT (bloquant)
    radio.startReceive();

    // Envoi MQTT
    sendToServer(plaintext, rssi);

  } else {
    Serial.printf("Erreur reception: %d\n", state);
    radio.startReceive();
  }
}
