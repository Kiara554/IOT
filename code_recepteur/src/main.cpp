#include <Arduino.h>
#include <RadioLib.h>
#include <Wire.h>
#include <SSD1306Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
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
// WiFi
// ============================================================
void connectWiFi() {
  Serial.printf("Connexion WiFi à %s...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi connecté — IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nEchec WiFi");
  }
}

void sendToServer(const String& payload, int rssi) {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    return;
  }
  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");
  String body = "{\"payload\":\"" + payload + "\",\"rssi\":" + rssi + "}";
  int code = http.POST(body);
  if (code > 0) {
    Serial.printf("HTTP POST OK: %d\n", code);
  } else {
    Serial.printf("HTTP POST erreur: %s\n", http.errorToString(code).c_str());
  }
  http.end();
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
    Serial.printf("RECU: %s | RSSI: %d dBm\n", rxData.c_str(), rssi);

    // OLED — mise à jour immédiate avant tout traitement long
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.drawString(0, 0, "Donnees Recues:");
    display.drawString(0, 20, rxData);
    display.drawString(0, 45, "Signal: " + String(rssi) + " dBm");
    display.display();

    // Remettre le LoRa en écoute AVANT le HTTP POST (bloquant)
    radio.startReceive();

    // Envoi WiFi (peut prendre ~500ms, LoRa écoute déjà)
    sendToServer(rxData, rssi);
  } else {
    Serial.printf("Erreur reception: %d\n", state);
    radio.startReceive();
  }
}
