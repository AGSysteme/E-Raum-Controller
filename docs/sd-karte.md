# SD-Karte: Ordnerstruktur und Test

## Ordnerstruktur

```
SD-Karte (FAT32)
├── AUDIO/
│   ├── 0000.WAV        Testansage (ID 0)
│   ├── 0001.WAV        Ansagen, 4-stellige ID
│   └── ...
├── CONFIG/
│   └── CONFIG.JSON     Geräte-ID, Pin-Zuordnung (später vom Server)
└── LOG/
    ├── ELA.LOG         aktuelles Ereignisprotokoll
    └── ELA_OLD.LOG     vorherige Datei nach Rotation
```

## Regeln

- **8.3-Dateinamen:** maximal 8 Zeichen plus 3 für die Endung. Die Ansagen-IDs sind 4-stellig und passen immer.
- **Audioformat:** WAV, PCM 16 Bit signed, mono, 16 kHz (passend zum PCM5102A, siehe `app/docs/mqtt.md`).
- **Log-Rotation:** Ab `LOG_MAX_BYTES` (64 KB) wird `ELA.LOG` nach `ELA_OLD.LOG` verschoben. Dadurch wächst die Datei nicht unbegrenzt.
- **Konfiguration:** Der Controller liest `CONFIG.JSON` nur beim Start. Das Format legen wir mit dem Server fest.

## SD-Test (src/main.cpp)

Der Sketch prüft die Karte in dieser Reihenfolge:

1. Karte einbinden mit `SD_SPI_FREQ` (4 MHz, bewusst niedrig gewählt).
2. Fehlende Ordner `AUDIO`, `CONFIG` und `LOG` anlegen.
3. `AUDIO/0000.WAV` auf einen gültigen RIFF/WAVE-Header prüfen.
4. Die Datei `SD_TEST_ROUNDS` mal (20×) komplett lesen. Dabei werden Lesezeit und Fehler gezählt.
5. Das Ergebnis wird auf dem USB-Monitor ausgegeben und in `LOG/ELA.LOG` geschrieben.

### Vorbereitung

1. Die Datei `/opt/ela/audio/system/testdurchsage.wav` auf die SD-Karte kopieren.
2. Auf der Karte in den Ordner `AUDIO` legen und in `0000.WAV` umbenennen.
3. Die Karte in das Modul stecken und den Strom einschalten.

### Erwartete Ausgabe

```
Karte erkannt: SDHC, <Größe> MB, Takt 4000000 Hz
Start SD-Test, Takt 4000000 Hz
0000.WAV gültig, 195... Bytes
Runde  1: ... ms
...
Ergebnis: 20/20 ok, Fehler: 0, Mittel ... ms, langsamste ... ms, ca. ... KB/s
```

### Bewertung

- **20/20 ok:** Die Aussetzer kamen von den Leitungen oder der Versorgung, nicht von der Software.
- **Fehler, aber nicht bei jeder Runde:** Wahrscheinlich Leitungsführung oder Spannungseinbrüche. Kabel kürzen und einen Elektrolytkondensator von etwa 100 µF direkt am SD-Modul ergänzen.
- **Fehler bei jeder Runde:** Verdrahtung prüfen (CS, SCK, MOSI, MISO) und die Karte mit einem anderen Lesegerät testen.

## Status

Der Sketch wurde auf dem Server nicht kompiliert, weil PlatformIO dort nicht installiert ist. Das Kompilieren und Hochladen erfolgt in VS Code auf dem Windows-PC.
