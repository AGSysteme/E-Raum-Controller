#pragma once

// ---------------------------------------------------------------
// Konfiguration E-Raum-Controller (Olimex ESP32-EVB)
// Pinbelegung: docs/pinbelegung.md
// Protokoll: app/docs/mqtt.md (Repo ela)
// ---------------------------------------------------------------

// Geräte-ID: muss der device_id in der ELA-Datenbank entsprechen
#define ELA_DEVICE_ID "eraum-1"

// MQTT-Broker (Mosquitto auf dem Server). Benutzer und Passwort stehen in
// include/secrets.h (nicht im Repo, siehe secrets.h.example).
#include "secrets.h"
#define MQTT_HOST "192.168.3.10"   // TODO: IP des Servers eintragen
#define MQTT_PORT 1883

// Relais (aus Beispiel RELAY.ino). HIGH = eingeschaltet (Annahme, am Relais prüfen)
#define RELAY1_PIN 32
#define RELAY2_PIN 33
#define RELAY_ON HIGH
#define RELAY_OFF LOW
#define RELAY_COUNT 2

// Taster (nur Eingang, Pull-up auf der Platine)
#define BUTTON_PIN 34

// microSD im 1-Bit-SD_MMC-Modus (CLK 14, CMD 15, DAT0 2)
#define SD_MOUNT_POINT "/sd"
#define SD_1BIT_MODE true

// I²S-Ausgang für PCM5102A (Pins frei, siehe docs/pinbelegung.md)
#define I2S_BCK_PIN 17
#define I2S_WS_PIN 16
#define I2S_DOUT_PIN 4
#define AUDIO_SAMPLE_RATE 16000

// Sicherheitsreserve nach der Ansagendauer (ms), siehe mqtt.md
#define WATCHDOG_MARGIN_MS 5000

// Status-Heartbeat (ms)
#define STATUS_INTERVAL_MS 30000

// Dateien auf der SD-Karte (wie in esp32-controller/docs/sd-karte.md)
#define AUDIO_DIR "/AUDIO"
#define LOG_FILE "/LOG/ELA.LOG"
