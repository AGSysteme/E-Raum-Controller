#include <Arduino.h>
#include <ETH.h>
#include <SD_MMC.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include "config.h"

// ---------------------------------------------------------------
// E-Raum-Controller
//
//  - Ethernet (Olimex ESP32-EVB, Standard-Pins des Boards)
//  - MQTT: Befehle empfangen (cmd, stop), Quittungen senden (ack),
//          Heartbeat (status, retained, Last-Will offline)
//  - SD-Karte (1-Bit): AUDIO/NNNN.WAV, CONFIG/, LOG/ELA.LOG
//  - Ansagen per HTTP nachladen, wenn sie fehlen
//  - Audioausgabe: PWM-Test auf GPIO 4 (AUDIO_BACKEND_PWM) oder I²S (PCM5102A)
//  - Zwei Relais: Ausgang 1 = Relais 1, Ausgang 2 = Relais 2
//
// Protokoll: app/docs/mqtt.md. Struktur der SD-Karte: docs/sd-karte.md.
// Wiedergabe läuft in einer eigenen Task (Kern 0), damit MQTT (Kern 1)
// während der Ansage weiterläuft und ein "stop" sofort wirkt.
// SD_MMC-Pfade sind relativ zur Kartenwurzel (der Mountpoint wird von der Bibliothek gesetzt).
// ---------------------------------------------------------------

WiFiClient netClient;
PubSubClient mqtt(netClient);

static String topicCmd;
static String topicStop;
static String topicAck;
static String topicStatus;

struct AckMsg {
    uint32_t id;
    char state[12];
    char detail[96];
};

struct PlayJob {
    uint32_t id;
    uint32_t audio;
    bool relayOn[RELAY_COUNT];
    uint32_t maxMs;
    char url[200];
};

static QueueHandle_t ackQueue;
static QueueHandle_t logQueue;
static volatile bool busy = false;
static volatile bool stopRequested = false;
static volatile uint32_t jobDeadline = 0;
static unsigned long lastStatusMs = 0;

// ---------- Logging: Tasks legen Zeilen in eine Warteschlange, die Hauptschleife schreibt ----------

static void logLine(const String &msg) {
    Serial.println(msg);
    String *copy = new String(msg);
    if (xQueueSend(logQueue, &copy, 0) != pdTRUE) {
        delete copy;  // Warteschlange voll: Zeile verwerfen
    }
}

static void writeLogLine(const String &msg) {
    File current = SD_MMC.open(LOG_FILE, FILE_READ);
    if (current) {
        size_t size = current.size();
        current.close();
        if (size > LOG_MAX_BYTES) {
            SD_MMC.remove(LOG_OLD_FILE);
            SD_MMC.rename(LOG_FILE, LOG_OLD_FILE);
        }
    }

    File log = SD_MMC.open(LOG_FILE, FILE_APPEND);
    if (log) {
        log.printf("%lu %s\n", millis(), msg.c_str());
        log.close();
    }
}

static void flushLog() {
    String *msg;
    while (xQueueReceive(logQueue, &msg, 0) == pdTRUE) {
        writeLogLine(*msg);
        delete msg;
    }
}

// ---------- Relais ----------

static void setRelay(int n, bool on) {
    int pin = (n == 1) ? RELAY1_PIN : RELAY2_PIN;
    digitalWrite(pin, on ? RELAY_ON : RELAY_OFF);
}

static void setAllRelays(bool on) {
    setRelay(1, on);
    setRelay(2, on);
}

// ---------- Quittungen (werden im Hauptloop veröffentlicht) ----------

static void queueAck(uint32_t id, const char *state, const char *detail = "") {
    AckMsg msg;
    msg.id = id;
    strlcpy(msg.state, state, sizeof(msg.state));
    strlcpy(msg.detail, detail, sizeof(msg.detail));
    xQueueSend(ackQueue, &msg, 0);

    String line = String("ack ") + String((unsigned long)id) + " " + state;
    if (detail[0] != '\0') {
        line += String(" ") + detail;
    }
    logLine(line);
}

static void publishAck(const AckMsg &msg) {
    JsonDocument doc;
    doc["id"] = msg.id;
    doc["state"] = msg.state;
    if (msg.detail[0] != '\0') {
        doc["detail"] = msg.detail;
    }

    char out[200];
    size_t n = serializeJson(doc, out, sizeof(out));
    mqtt.publish(topicAck.c_str(), out, n);
}

// ---------- Audio: Download per HTTP ----------

static bool downloadFile(const char *url, const char *path) {
    HTTPClient http;
    http.begin(url);
    int code = http.GET();

    if (code != HTTP_CODE_OK) {
        logLine(String("download ") + url + ": HTTP " + String(code));
        http.end();
        return false;
    }

    // Temporärer Name im 8.3-Format (FAT ohne lange Namen)
    String tmp = String(AUDIO_DIR) + "/DL.TMP";
    if (!SD_MMC.exists(AUDIO_DIR)) {
        SD_MMC.mkdir(AUDIO_DIR);
    }
    SD_MMC.remove(tmp.c_str());
    File out = SD_MMC.open(tmp.c_str(), FILE_WRITE);
    if (!out) {
        logLine(String("download: konnte ") + tmp + " nicht schreiben");
        http.end();
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    int expected = http.getSize();
    uint8_t buf[1024];
    int total = 0;
    unsigned long lastData = millis();

    while (http.connected() && (expected < 0 || total < expected)) {
        size_t avail = stream->available();
        if (avail) {
            int n = stream->readBytes(buf, min(avail, sizeof(buf)));
            out.write(buf, n);
            total += n;
            lastData = millis();
        } else if (millis() - lastData > 10000) {
            break;  // 10 s ohne Daten: abbrechen
        } else {
            delay(1);
        }
    }

    out.close();
    http.end();

    if (expected >= 0 && total != expected) {
        SD_MMC.remove(tmp.c_str());
        logLine(String("download unvollständig: ") + String(total) + " von " + String(expected) + " Bytes");
        return false;
    }

    SD_MMC.remove(path);
    bool ok = SD_MMC.rename(tmp.c_str(), path);
    logLine(String("download ok: ") + path + " (" + String(total) + " Bytes)");
    return ok;
}

// ---------- Audio: WAV-Header ----------

// Liest den Header bis zum data-Chunk. Gibt die Datenlänge zurück, 0 bei Fehler.
static uint32_t wavDataStart(File &f) {
    uint8_t hdr[12];
    if (f.read(hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        return 0;
    }

    while (f.available() >= 8) {
        uint8_t ck[8];
        f.read(ck, 8);
        uint32_t size = ck[4] | (ck[5] << 8) | (ck[6] << 16) | ((uint32_t)ck[7] << 24);

        if (memcmp(ck, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            f.read(fmt, min<uint32_t>(size, 16));
            uint16_t format = fmt[0] | (fmt[1] << 8);
            uint16_t channels = fmt[2] | (fmt[3] << 8);
            uint32_t rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            uint16_t bits = fmt[14] | (fmt[15] << 8);
            if (format != 1 || channels != 1 || bits != 16 || rate != AUDIO_SAMPLE_RATE) {
                logLine(String("WAV-Format nicht unterstützt: fmt=") + String(format) + " ch=" + String(channels)
                        + " bits=" + String(bits) + " rate=" + String((unsigned long)rate));
                return 0;
            }
            if (size > 16) {
                f.seek(f.position() + (size - 16));
            }
            if (size & 1) {
                f.seek(f.position() + 1);
            }
        } else if (memcmp(ck, "data", 4) == 0) {
            return size;
        } else {
            f.seek(f.position() + size + (size & 1));
        }
    }
    return 0;
}

// ---------- Audioausgabe ----------

#if AUDIO_BACKEND_PWM
// Ein 16-Bit-Sample als 8-Bit-PWM ausgeben, mit fester Taktung (16 kHz)
static void outputSamplesPwm(const uint8_t *buf, size_t bytes, uint32_t &nextUs) {
    for (size_t i = 0; i + 1 < bytes; i += 2) {
        int16_t s = (int16_t)(buf[i] | (buf[i + 1] << 8));
        uint8_t v = (uint8_t)((s + 32768) >> 8);
        ledcWrite(AUDIO_PWM_PIN, v);
        while ((int32_t)(micros() - nextUs) < 0) {
        }
        nextUs += 62;  // 1e6 / 16000 Hz, ergibt ca. 0,8 % Tempoabweichung
    }
}
#endif

// Spielt die Datei ab. Bricht bei stopRequested oder nach maxMs ab.
static bool playWav(File &f, uint32_t maxMs) {
    uint32_t dataSize = wavDataStart(f);
    if (dataSize == 0) {
        return false;
    }

    static uint8_t buf[512];
    uint32_t remaining = dataSize;
    uint32_t start = millis();
#if AUDIO_BACKEND_PWM
    uint32_t nextUs = micros();
#endif

    while (remaining > 0 && !stopRequested && (millis() - start) < maxMs) {
        size_t want = min<size_t>(remaining, sizeof(buf));
        int n = f.read(buf, want);
        if (n <= 0) {
            break;
        }
#if AUDIO_BACKEND_PWM
        outputSamplesPwm(buf, n, nextUs);
#else
        size_t written = 0;
        i2s_write(I2S_NUM_0, buf, n, &written, portMAX_DELAY);
#endif
        remaining -= n;
    }

#if AUDIO_BACKEND_PWM
    ledcWrite(AUDIO_PWM_PIN, 128);  // Mittelstellung, kein Knacken am Ende
#endif
    if (!stopRequested) {
        delay(50);
    }
    return true;
}

// ---------- Wiedergabe-Task ----------

static void playTask(void *param) {
    PlayJob *job = (PlayJob *)param;

    char path[64];
    snprintf(path, sizeof(path), "%s/%04u.WAV", AUDIO_DIR, (unsigned)job->audio);

    if (!SD_MMC.exists(path)) {
        if (job->url[0] == '\0' || !downloadFile(job->url, path)) {
            queueAck(job->id, "error", "Ansagedatei fehlt und Download fehlgeschlagen");
            goto finish;
        }
    }

    {
        File f = SD_MMC.open(path, FILE_READ);
        if (!f) {
            queueAck(job->id, "error", "Ansagedatei nicht lesbar");
            goto finish;
        }

        for (int i = 0; i < RELAY_COUNT; i++) {
            setRelay(i + 1, job->relayOn[i]);
        }
        queueAck(job->id, "started");

        bool ok = playWav(f, job->maxMs);
        f.close();

        setAllRelays(false);
        if (!ok) {
            queueAck(job->id, "error", "WAV-Datei ungültig");
        } else if (stopRequested) {
            queueAck(job->id, "done", "abgebrochen");
        } else {
            queueAck(job->id, "done");
        }
    }

finish:
    setAllRelays(false);
    stopRequested = false;
    busy = false;
    delete job;
    vTaskDelete(NULL);
}

// ---------- MQTT ----------

static void onMessage(char *topic, byte *payload, unsigned int length) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, length)) {
        logLine(String("JSON ungültig auf ") + topic);
        return;
    }

    uint32_t id = doc["id"] | 0;
    String t(topic);

    if (t == topicStop) {
        stopRequested = true;
        if (!busy) {
            setAllRelays(false);
        }
        queueAck(id, "done", "stop");
        return;
    }

    if (t != topicCmd) {
        return;
    }

    if (busy) {
        queueAck(id, "error", "Controller ist beschäftigt");
        return;
    }

    PlayJob job = {};
    job.id = id;
    job.audio = doc["audio"] | 0;
    job.maxMs = doc["max_ms"] | 0;
    uint32_t durationMs = doc["duration_ms"] | 0;
    const char *url = doc["url"] | "";
    strlcpy(job.url, url, sizeof(job.url));

    if (durationMs == 0 || job.maxMs < durationMs) {
        queueAck(id, "error", "duration_ms/max_ms ungültig");
        return;
    }
    if (job.audio > 9999) {
        queueAck(id, "error", "Ansagen-ID ungültig");
        return;
    }

    JsonArrayConst pins = doc["pins"];
    for (JsonVariantConst p : pins) {
        int pin = p | 0;
        if (pin < 1 || pin > RELAY_COUNT) {
            char detail[64];
            snprintf(detail, sizeof(detail), "Ausgang %d nicht vorhanden", pin);
            queueAck(id, "error", detail);
            return;
        }
        job.relayOn[pin - 1] = true;
    }

    PlayJob *copy = new PlayJob(job);
    busy = true;
    stopRequested = false;
    jobDeadline = millis() + job.maxMs;
    // Kern 0: die Hauptschleife auf Kern 1 bleibt für MQTT frei
    xTaskCreatePinnedToCore(playTask, "play", 8192, copy, 1, NULL, 0);
}

static void mqttConnect() {
    while (!mqtt.connected()) {
        logLine(String("MQTT verbinden mit ") + MQTT_HOST + ":" + String(MQTT_PORT));
        bool ok = mqtt.connect(
            ELA_DEVICE_ID, MQTT_USER, MQTT_PASS,
            topicStatus.c_str(), 1, true, "{\"state\":\"offline\"}");

        if (ok) {
            mqtt.subscribe(topicCmd.c_str(), 1);
            mqtt.subscribe(topicStop.c_str(), 1);
            mqtt.publish(topicStatus.c_str(), "{\"state\":\"online\"}", true);
            logLine("MQTT verbunden");
        } else {
            logLine(String("MQTT-Fehler rc=") + String(mqtt.state()) + ", neuer Versuch in 3 s");
            delay(3000);
        }
    }
}

// ---------- Setup / Loop ----------

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== E-Raum-Controller ===");

    logQueue = xQueueCreate(32, sizeof(String *));
    ackQueue = xQueueCreate(16, sizeof(AckMsg));

    // Relais sofort aus, bevor sie als Ausgang geschaltet werden
    digitalWrite(RELAY1_PIN, RELAY_OFF);
    digitalWrite(RELAY2_PIN, RELAY_OFF);
    pinMode(RELAY1_PIN, OUTPUT);
    pinMode(RELAY2_PIN, OUTPUT);
    pinMode(BUTTON_PIN, INPUT);

    if (!SD_MMC.begin(SD_MOUNT_POINT, SD_1BIT_MODE)) {
        Serial.println("FEHLER: SD-Karte nicht erkannt");
    } else {
        Serial.printf("SD eingebunden, %llu MB\n", SD_MMC.totalBytes() / (1024ULL * 1024ULL));
        // Ordnerstruktur nach docs/sd-karte.md
        const char *dirs[] = {AUDIO_DIR, CONFIG_DIR, LOG_DIR};
        for (const char *d : dirs) {
            if (!SD_MMC.exists(d)) {
                bool made = SD_MMC.mkdir(d);
                Serial.printf("Ordner %s angelegt: %s\n", d, made ? "ja" : "NEIN");
            } else {
                Serial.printf("Ordner %s vorhanden\n", d);
            }
        }
        // Prüfung: Ordner-Inhalt und Logdatei-Größe ausgeben
        File audio = SD_MMC.open(AUDIO_DIR);
        for (File e = audio.openNextFile(); e; e = audio.openNextFile()) {
            Serial.printf("  %s/%s (%u Bytes)\n", AUDIO_DIR, e.name(), (unsigned)e.size());
        }
        audio.close();
        File logf = SD_MMC.open(LOG_FILE, FILE_READ);
        Serial.printf("Logdatei %s: %s\n", LOG_FILE, logf ? "vorhanden" : "fehlt (wird beim ersten Eintrag angelegt)");
        if (logf) {
            Serial.printf("  Größe: %u Bytes\n", (unsigned)logf.size());
            logf.close();
        }
    }

#if AUDIO_BACKEND_PWM
    // Test-Ausgabe: 8-Bit-PWM auf GPIO 4, Tiefpass (2,2 kΩ + 10 nF) davor
    // Arduino-ESP32 3.x: ledcAttach(Pin, Frequenz, Auflösung), ledcWrite über den Pin
    ledcAttach(AUDIO_PWM_PIN, AUDIO_PWM_FREQ, 8);
    ledcWrite(AUDIO_PWM_PIN, 128);
#else
    i2s_config_t i2sConfig = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
    };
    i2s_pin_config_t i2sPins = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = I2S_BCK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_DOUT_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };
    i2s_driver_install(I2S_NUM_0, &i2sConfig, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &i2sPins);
#endif

    // Ethernet mit den Standard-Pins des Olimex-Boards
    ETH.begin();
    Serial.print("Ethernet: warte auf IP ");
    unsigned long t0 = millis();
    while (ETH.localIP() == IPAddress(0, 0, 0, 0) && millis() - t0 < 20000) {
        delay(250);
        Serial.print('.');
    }
    Serial.println();
    logLine(String("IP: ") + ETH.localIP().toString());

    String base = String("ela/") + ELA_DEVICE_ID;
    topicCmd = base + "/cmd";
    topicStop = base + "/stop";
    topicAck = base + "/ack";
    topicStatus = base + "/status";

    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setBufferSize(512);
    mqtt.setCallback(onMessage);

    logLine("Start, Audio-Ausgang: " AUDIO_BACKEND_NAME);
}

void loop() {
    if (!mqtt.connected()) {
        mqttConnect();
    }
    mqtt.loop();

    AckMsg ack;
    while (xQueueReceive(ackQueue, &ack, 0) == pdTRUE) {
        publishAck(ack);
    }

    // Log nur schreiben, wenn keine Wiedergabe auf die SD-Karte zugreift
    if (!busy) {
        flushLog();
    }

    // Sicherheitsnetz: nach max_ms müssen die Relais aus sein, auch wenn die Task hängt
    if (busy && millis() > jobDeadline + WATCHDOG_MARGIN_MS) {
        logLine("WATCHDOG: Relais werden freigegeben");
        setAllRelays(false);
        stopRequested = true;
    }

    if (millis() - lastStatusMs >= STATUS_INTERVAL_MS) {
        lastStatusMs = millis();
        mqtt.publish(topicStatus.c_str(), "{\"state\":\"online\"}", true);
    }
}
