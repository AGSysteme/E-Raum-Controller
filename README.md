# E-Raum-Controller (Olimex ESP32-EVB)

Firmware für den Olimex ESP32-EVB mit Ethernet. Der Controller

- empfängt Befehle über MQTT (`ela/<device_id>/cmd`, `stop`),
- spielt Ansagen von der microSD-Karte über einen I²S-DAC (PCM5102A) ab,
- lädt fehlende Ansagen per HTTP nach,
- schaltet zwei Relais (Ausgang 1 und 2).

Protokoll: `app/docs/mqtt.md` im Repo `ela`.
Pinbelegung und Quellen: `docs/pinbelegung.md`.

## Status

**Nicht kompiliert und nicht am Board getestet.** Die Pins für Ethernet, SD-Karte und Relais stammen aus Olimex-Schaltplan und -Beispielen. Die Wiedergabe (I²S) ist noch nicht geprüft.

- [x] Projektgerüst, Konfiguration, Pinbelegung dokumentiert
- [x] MQTT: cmd, stop, ack, status mit Last-Will
- [x] Relais über Ausgänge 1 und 2
- [x] SD-Karte (1-Bit) einbinden, Ordner /AUDIO
- [x] HTTP-Download fehlender Ansagen
- [ ] Erster Test am Board (Ethernet, MQTT, Relais klicken)
- [ ] Relais-Polarität bestätigen
- [ ] I²S-Wiedergabe testen (PCM5102A anschließen)
- [ ] Logdatei auf SD (Vorlage: `esp32-controller/docs/sd-karte.md`)

## Einrichtung

1. **Repo holen** (`git clone`), in VS Code öffnen, PlatformIO-Erweiterung installieren.
2. **Zugangsdaten:** `include/secrets.h.example` nach `include/secrets.h` kopieren und das Passwort eintragen. Die Datei bleibt lokal (.gitignore).
3. **Server-IP** in `include/config.h` bei `MQTT_HOST` eintragen.
4. **Datenbank:** Im Controller-Bereich der ELA-Oberfläche einen Controller mit der Geräte-ID `eraum-1` anlegen (Anbindung: LAN).
5. **Mosquitto:** Benutzer `eraum-1` anlegen und in der ACL auf `ela/eraum-1/#` beschränken:
   ```bash
   sudo mosquitto_passwd /etc/mosquitto/ela_passwd eraum-1
   ```
   In `/etc/mosquitto/ela_acl`:
   ```
   user eraum-1
   topic readwrite ela/eraum-1/#
   ```
   Danach `systemctl restart mosquitto`.
6. **SD-Karte:** Ordner `AUDIO` anlegen, Ansagen als `0000.WAV` usw. ablegen (16 Bit, mono, 16 kHz).
7. **Upload** in VS Code, Monitor öffnen (115200 Baud). Erwartet: „IP: …“ und „MQTT verbunden“.

## Befehle

Ansage mit Relais 1 (Ausgang 1) abspielen:
```json
{ "id": 7, "audio": 0, "pins": [1], "duration_ms": 6107, "max_ms": 11107 }
```

Quittungen erscheinen unter `ela/eraum-1/ack` mit `started`, `done` oder `error`.

## Offene Punkte

- **Relais-Polarität:** Die Beispiele nehmen HIGH = eingeschaltet an. Am Relais prüfen.
- **Download-URL:** Das Programm lädt eine fehlende Ansage nur, wenn der Befehl ein Feld `url` enthält. Der Server schickt das bisher nicht mit. Das ist ein Erweiterungspunkt in der Bridge.
- **I²S-Pins:** GPIO 17, 16 und 4 sind nach Schaltplan frei, müssen aber am Board noch bestätigt werden.
- **Audio-Ausgang:** Wie der PCM5102A an den Verstärker kommt (40-polige Leiste oder UEXT) ist noch offen.
- **Relais und Verstärker:** Zwei Relais sind nur für Netzspannung ausgelegt. Die Anschlüsse mit Fachkenntnis verdrahten.
