#pragma once

// ---------------------------------------------------------------
// Konfiguration E-Raum-Controller (Olimex ESP32-EVB)
// Pinbelegung: docs/pinbelegung.md
// Protokoll: app/docs/mqtt.md (Repo ela)
// SD-Struktur: docs/sd-karte.md
// ---------------------------------------------------------------

// Geräte-ID: muss der device_id in der ELA-Datenbank entsprechen
#define ELA_DEVICE_ID "eraum-1"

// MQTT-Broker (Mosquitto auf dem Server). Benutzer und Passwort stehen in
// include/secrets.h (nicht im Repo, siehe secrets.h.example).
#include "secrets.h"
#define MQTT_HOST "192.168.3.104"  // Server (Mosquitto)
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

// ---- Audioausgabe ----
// AUDIO_BACKEND_PWM = 1: Funktionstest über PWM auf GPIO 4 mit RC-Tiefpass
//                        (2,2 kΩ in Reihe, 10 nF nach GND), ohne PCM5102A
// AUDIO_BACKEND_PWM = 0: I²S-Ausgang für PCM5102A (sobald verfügbar)
#define AUDIO_BACKEND_PWM 1
#define AUDIO_BACKEND_I2S (!AUDIO_BACKEND_PWM)
#if AUDIO_BACKEND_PWM
#define AUDIO_BACKEND_NAME "PWM (Test, GPIO 4)"
#define AUDIO_PWM_PIN 4
#define AUDIO_PWM_CHANNEL 0
#define AUDIO_PWM_FREQ 78125      // Hz, 8 Bit Auflösung
#else
#define AUDIO_BACKEND_NAME "I2S (PCM5102A)"
#endif

// I²S-Pins für den PCM5102A (nur bei AUDIO_BACKEND_PWM = 0 verwendet)
#define I2S_BCK_PIN 17
#define I2S_WS_PIN 16
#define I2S_DOUT_PIN 5
#define AUDIO_SAMPLE_RATE 16000   // Hz, WAV: 16 Bit signed, mono

// Sicherheitsreserve nach der Ansagendauer (ms), siehe mqtt.md
#define WATCHDOG_MARGIN_MS 5000

// Status-Heartbeat (ms)
#define STATUS_INTERVAL_MS 30000

// SD-Ordnerstruktur (docs/sd-karte.md). Pfade relativ zur Kartenwurzel.
#define AUDIO_DIR "/AUDIO"
#define CONFIG_DIR "/CONFIG"
#define LOG_DIR "/LOG"
#define LOG_FILE "/LOG/ELA.LOG"
#define LOG_OLD_FILE "/LOG/ELA_OLD.LOG"
#define LOG_MAX_BYTES 65536       // ab dieser Größe wird ELA.LOG rotiert
