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
//  - SD-Karte (1-Bit): Ansagen in /AUDIO/NNNN.WAV, Download per HTTP
//  - I²S-DAC (PCM5102A): Wiedergabe von 16 Bit mono 16 kHz
//  - Zwei Relais: Ausgang 1 = Relais 1, Ausgang 2 = Relais 2
//
// Protokoll: app/docs/mqtt.md
// Die Wiedergabe läuft in einer eigenen Task, damit MQTT während der
// Ansage weiterläuft und ein "stop" sofort wirkt.
// ---------------------------------------------------------------

// SD_MMC-Pfade sind relativ zur Kartenwurzel (den Mountpoint setzt die Bibliothek selbst)

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
static volatile bool busy = false;
static volatile bool stopRequested = false;
static volatile uint32_t jobDeadline = 0;
static unsigned long lastStatusMs = 0;

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
    Serial.printf("ack %lu %s %s\n", (unsigned long)msg.id, msg.state, msg.detail);
}

// ---------- Audio: Download per HTTP ----------

static bool downloadFile(const char *url, const char *path) {
    HTTPClient http;
    http.begin(url);
    int code = http.GET();

    if (code != HTTP_CODE_OK) {
        Serial.printf("download %s: HTTP %d\n", url, code);
        http.end();
        return false;
    }

    // Temporärer Name im 8.3-Format (FAT ohne lange Namen): /sd/AUDIO/DL.TMP
    String tmp = String(AUDIO_DIR) + "/DL.TMP";
    if (!SD_MMC.exists(AUDIO_DIR)) {
        SD_MMC.mkdir(AUDIO_DIR);
    }
    SD_MMC.remove(tmp.c_str());
    File out = SD_MMC.open(tmp.c_str(), FILE_WRITE);
    if (!out) {
        Serial.printf("download: konnte %s nicht schreiben\n", tmp.c_str());
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
        Serial.printf("download unvollständig: %d von %d Bytes\n", total, expected);
        return false;
    }

    SD_MMC.remove(path);
    return SD_MMC.rename(tmp.c_str(), path);
}

// ---------- Audio: WAV-Wiedergabe ----------

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
                Serial.printf("WAV-Format nicht unterstützt: fmt=%u ch=%u bits=%u rate=%lu\n",
                              format, channels, bits, (unsigned long)rate);
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

// Spielt die Datei ab. Bricht bei stopRequested oder nach maxMs ab.
static bool playWav(File &f, uint32_t maxMs) {
    uint32_t dataSize = wavDataStart(f);
    if (dataSize == 0) {
        return false;
    }

    static uint8_t buf[512];
    uint32_t remaining = dataSize;
    uint32_t start = millis();

    while (remaining > 0 && !stopRequested && (millis() - start) < maxMs) {
        size_t want = min<size_t>(remaining, sizeof(buf));
        int n = f.read(buf, want);
        if (n <= 0) {
            break;
        }
        size_t written = 0;
        i2s_write(I2S_NUM_0, buf, n, &written, portMAX_DELAY);
        remaining -= n;
    }

    // Ausklingen lassen, damit der DMA-Puffer nicht abgeschnitten wird
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
        Serial.printf("JSON ungültig auf %s\n", topic);
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
    xTaskCreatePinnedToCore(playTask, "play", 8192, copy, 1, NULL, 1);
}

static void mqttConnect() {
    while (!mqtt.connected()) {
        Serial.printf("MQTT verbinden mit %s:%d ...\n", MQTT_HOST, MQTT_PORT);
        bool ok = mqtt.connect(
            ELA_DEVICE_ID, MQTT_USER, MQTT_PASS,
            topicStatus.c_str(), 1, true, "{\"state\":\"offline\"}");

        if (ok) {
            mqtt.subscribe(topicCmd.c_str(), 1);
            mqtt.subscribe(topicStop.c_str(), 1);
            mqtt.publish(topicStatus.c_str(), "{\"state\":\"online\"}", true);
            Serial.println("MQTT verbunden");
        } else {
            Serial.printf("MQTT-Fehler rc=%d, neuer Versuch in 3 s\n", mqtt.state());
            delay(3000);
        }
    }
}

// ---------- Setup / Loop ----------

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== E-Raum-Controller ===");

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
        String audioDir = String(AUDIO_DIR);
        if (!SD_MMC.exists(audioDir.c_str())) {
            SD_MMC.mkdir(audioDir.c_str());
        }
    }

    // I²S für den PCM5102A
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

    // Ethernet mit den Standard-Pins des Olimex-Boards
    ETH.begin();
    Serial.print("Ethernet: warte auf IP ");
    unsigned long t0 = millis();
    while (ETH.localIP() == IPAddress(0, 0, 0, 0) && millis() - t0 < 20000) {
        delay(250);
        Serial.print('.');
    }
    Serial.println();
    Serial.printf("IP: %s\n", ETH.localIP().toString().c_str());

    String base = String("ela/") + ELA_DEVICE_ID;
    topicCmd = base + "/cmd";
    topicStop = base + "/stop";
    topicAck = base + "/ack";
    topicStatus = base + "/status";

    ackQueue = xQueueCreate(16, sizeof(AckMsg));

    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setBufferSize(512);
    mqtt.setCallback(onMessage);
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

    // Sicherheitsnetz: nach max_ms müssen die Relais aus sein, auch wenn die Task hängt
    if (busy && millis() > jobDeadline + WATCHDOG_MARGIN_MS) {
        Serial.println("WATCHDOG: Relais werden freigegeben");
        setAllRelays(false);
        stopRequested = true;
    }

    if (millis() - lastStatusMs >= STATUS_INTERVAL_MS) {
        lastStatusMs = millis();
        mqtt.publish(topicStatus.c_str(), "{\"state\":\"online\"}", true);
    }
}
