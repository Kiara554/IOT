#include "LoRaWan_APP.h"
#include <Wire.h>
#include <Adafruit_BME280.h>
#include <Adafruit_Sensor.h>
#include "mbedtls/aes.h"

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

// ============================================================
// AES-128-CBC — chiffrement
// DOIT correspondre à AES_KEY_BYTES / AES_IV_BYTES dans config.h du récepteur
// Clé : "smartcesi2024key" | IV : "SmartCESI_IV_202"
// ============================================================
static const uint8_t aes_key[16] = {
  0x73,0x6d,0x61,0x72,0x74,0x63,0x65,0x73,
  0x69,0x32,0x30,0x32,0x34,0x6b,0x65,0x79
};
static const uint8_t aes_iv[16] = {
  0x53,0x6d,0x61,0x72,0x74,0x43,0x45,0x53,
  0x49,0x5f,0x49,0x56,0x5f,0x32,0x30,0x32
};

// plaintext max ~80 chars → padded max 96 bytes → hex max 192 chars + null
#define PLAIN_BUF_SIZE  96
#define TX_BUF_SIZE    200

Adafruit_BME280 bme;
static RadioEvents_t RadioEvents;

static char  plaintext[PLAIN_BUF_SIZE];
static char  txpacket[TX_BUF_SIZE];
static uint32_t seqNum = 0;

// ============================================================
// Chiffrement AES-128-CBC avec padding PKCS7.
// Écrit le ciphertext en hex dans outHex (taille outHexSize).
// Retourne true si succès.
// ============================================================
bool encryptAES128(const char* plain, char* outHex, size_t outHexSize) {
  const size_t plainLen = strlen(plain);

  // Padding PKCS7 : on complète jusqu'au prochain multiple de 16
  const uint8_t pad       = 16 - (plainLen % 16);
  const size_t  paddedLen = plainLen + pad;

  if (paddedLen * 2 + 1 > outHexSize) {
    Serial.println("[AES] Erreur: buffer trop petit");
    return false;
  }

  uint8_t buf[PLAIN_BUF_SIZE];  // taille fixe, >= paddedLen
  memcpy(buf, plain, plainLen);
  memset(buf + plainLen, pad, pad);

  // Chiffrement AES-128-CBC
  uint8_t cipher[PLAIN_BUF_SIZE];
  uint8_t iv[16];
  memcpy(iv, aes_iv, 16);  // copie : mbedtls modifie l'IV en place

  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, aes_key, 128);
  int ret = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, paddedLen, iv, buf, cipher);
  mbedtls_aes_free(&ctx);

  if (ret != 0) {
    Serial.printf("[AES] Erreur mbedtls: %d\n", ret);
    return false;
  }

  // Encodage hex
  for (size_t i = 0; i < paddedLen; i++) {
    sprintf(outHex + i * 2, "%02x", cipher[i]);
  }
  outHex[paddedLen * 2] = '\0';
  return true;
}

void setup() {
  Serial.begin(115200);

  Mcu.begin(HELTEC_BOARD, SLOW_CLK_TPYE);

  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
  delay(500);

  Wire.begin(40, 41); // SDA=40, SCL=41
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
  float t   = bme.readTemperature();
  float h   = bme.readHumidity();
  float p   = bme.readPressure() / 100.0F;
  int   gaz = analogRead(MQ_PIN);
  int   pir = digitalRead(PIR_PIN);

  if (isnan(t)) t = 0.0;
  if (isnan(h)) h = 0.0;
  if (isnan(p)) p = 0.0;

  // Construction du texte clair (format attendu par le serveur)
  snprintf(plaintext, sizeof(plaintext),
           "T:%.1f,H:%.1f,P:%.0f,G:%d,Presence:%d",
           t, h, p, gaz, pir);

  Serial.printf("[#%lu] Clair: %s\n", seqNum, plaintext);

  // Chiffrement AES-128-CBC
  if (!encryptAES128(plaintext, txpacket, sizeof(txpacket))) {
    Serial.println("Chiffrement echoue — trame ignoree");
    delay(5000);
    return;
  }

  Serial.printf("[#%lu] Chiffre (%d chars): %s\n", seqNum, strlen(txpacket), txpacket);

  Radio.Send((uint8_t*)txpacket, strlen(txpacket));
  seqNum++;

  delay(100);
  Radio.IrqProcess();
  delay(5000);
}
