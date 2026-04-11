#include "LoRaWan_APP.h"
#include <Wire.h>
#include <Adafruit_BME280.h>
#include <Adafruit_Sensor.h>

// --- CONFIGURATION PINS ---
#define PIR_PIN 33
#define MQ_PIN 1
#define Vext 18

// --- CONFIGURATION LORA ---
#define RF_FREQUENCY 868000000
#define TX_OUTPUT_POWER 14
#define LORA_BANDWIDTH 0
#define LORA_SPREADING_FACTOR 7
#define LORA_CODINGRATE 1

Adafruit_BME280 bme;
static RadioEvents_t RadioEvents;
char txpacket[100];

void setup() {
  Serial.begin(115200);
  
  Mcu.begin(HELTEC_BOARD, SLOW_CLK_TPYE);

  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
  delay(500);

  Wire.begin(40, 41); // SDA=40, SCL=41 — ton câblage
  
  if (!bme.begin(0x77)) {
    Serial.println("BME280 non trouve ! Verif cablage.");
  } else {
    Serial.println("BME280 OK !");
  }

  Radio.Init(&RadioEvents);
  Radio.SetChannel(RF_FREQUENCY);
  Radio.SetTxConfig(MODEM_LORA, TX_OUTPUT_POWER, 0, LORA_BANDWIDTH,
                    LORA_SPREADING_FACTOR, LORA_CODINGRATE, 8,
                    false, true, 0, 0, false, 3000);

  pinMode(PIR_PIN, INPUT);
  Serial.println("Emetteur pret !");
}

void loop() {
  float t = bme.readTemperature();
  float h = bme.readHumidity();
  float p = bme.readPressure() / 100.0F;
  int gaz = analogRead(MQ_PIN);
  int pres = digitalRead(PIR_PIN);

  if (isnan(t)) t = 0.0;
  if (isnan(h)) h = 0.0;
  if (isnan(p)) p = 0.0;

  sprintf(txpacket, "T:%.1f,H:%.1f,P:%.0f,G:%d,Presence:%d", t, h, p, gaz, pres);
  
  Serial.print("Envoi : ");
  Serial.println(txpacket);

  Radio.Send((uint8_t *)txpacket, strlen(txpacket));
  delay(100);
  Radio.IrqProcess();

  delay(5000);
}