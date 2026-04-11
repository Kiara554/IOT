#include "LoRaWan_APP.h"
#include "HT_SSD1306Wire.h"

#define RF_FREQUENCY 868000000
#define LORA_BANDWIDTH 0
#define LORA_SPREADING_FACTOR 7
#define LORA_CODINGRATE 1

// On renomme 'display' en 'myDisplay' pour éviter le conflit avec la bibliothèque
SSD1306Wire myDisplay(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);
static RadioEvents_t RadioEvents;

void OnRxDone(uint8_t *payload, uint16_t size, int16_t rssi, int8_t snr) {
  char rxpacket[64];
  memcpy(rxpacket, payload, size);
  rxpacket[size] = '\0';

  Serial.printf("RECU: %s | RSSI: %d dBm\n", rxpacket, rssi);

  // Utilisation du nouveau nom 'myDisplay'
  myDisplay.clear();
  myDisplay.setFont(ArialMT_Plain_10);
  myDisplay.drawString(0, 0, "Donnees Recues:");

  myDisplay.setFont(ArialMT_Plain_10); // Taille plus petite pour que tout rentre
  myDisplay.drawString(0, 20, rxpacket);

  myDisplay.setFont(ArialMT_Plain_10);
  myDisplay.drawString(0, 45, "Signal: " + String(rssi) + " dBm");
  myDisplay.display();

  Radio.Rx(0);
}

void setup() {
  Serial.begin(115200);
  Mcu.begin(HELTEC_BOARD, SLOW_CLK_TPYE);

  // Activation de l'alimentation de l'écran
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
  delay(100);

  // Initialisation avec le nouveau nom
  myDisplay.init();
  myDisplay.setFont(ArialMT_Plain_10);
  myDisplay.drawString(0, 0, "En attente LoRa...");
  myDisplay.display();

  RadioEvents.RxDone = OnRxDone;
  Radio.Init(&RadioEvents);
  Radio.SetChannel(RF_FREQUENCY);
  Radio.SetRxConfig(MODEM_LORA, LORA_BANDWIDTH, LORA_SPREADING_FACTOR, LORA_CODINGRATE, 0, 8, 0, false, 0, true, 0, 0, false, true);

  Radio.Rx(0);
}

void loop() {
  Radio.IrqProcess();
}
