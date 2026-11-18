# Pinbelegung: Olimex ESP32-EVB (E-Raum-Controller)

Quellen:
- Schaltplan `HARDWARE/REV-L/ESP32-EVB_Rev_L.pdf` (GitHub OLIMEX/ESP32-EVB)
- Beispiel `SOFTWARE/Arduino IDE examples/RELAY/RELAY.ino`
- Beispiel `SOFTWARE/Arduino IDE examples/MQQT-demos/ESP32-EVB_MQTT_GPIO_ETH/ESP32-EVB_MQTT_GPIO_ETH.ino`
- Handbuch `DOCS/ESP32-EVB-user-manual.pdf` (SD-Karte im 1-Bit-Modus)

## Belegt durch die Platine

| Funktion | GPIO | Quelle | Hinweis |
|---|---|---|---|
| Relais 1 | 32 | RELAY.ino | Polarität im Beispiel nicht eindeutig; Test am Relais nötig |
| Relais 2 | 33 | RELAY.ino | wie Relais 1 |
| Taster (User) | 34 | RELAY.ino | nur Eingang, externer Pull-up auf der Platine |
| Ethernet (RMII) | 19, 21, 22, 25, 26, 27 | Schaltplan (Netznamen TXD0, TX_EN, TXD1, RXD0, RXD1, CRS_DV) | nicht ändern |
| Ethernet MDC / MDIO | 23 / 18 | Standard LAN8710A (im Beispiel nicht explizit) | nicht ändern |
| Ethernet Takt | 0 (Eingang vom PHY) | Schaltplan (EMAC_TX_CLK) | nicht ändern |
| microSD (1-Bit) | CLK 14, CMD 15, DAT0 2 | Schaltplan, Handbuch (1-Bit-Modus) | `SD_MMC.begin(…, true)` |

## Frei für eigene Nutzung (vor dem Löten prüfen)

| Funktion geplant | GPIO | Hinweis |
|---|---|---|
| I²S BCK (PCM5102A) | 17 | nicht vom Ethernet belegt |
| I²S WS (PCM5102A) | 16 | nicht vom Ethernet belegt |
| I²S DIN (PCM5102A) | 5 | Strapping-Pin (SDIO-Timing), als Ausgang nach dem Booten unkritisch |
| PWM-Test-Audio | 4 | Test ohne PCM5102A: 8-Bit-PWM, 2,2 kΩ in Reihe, 10 nF nach GND |

Der interne DAC (GPIO 25/26) ist nicht nutzbar: Auf diesem Board sind das die Ethernet-Datenleitungen RXD0/RXD1.

Nicht verwenden:
- GPIO 3 (U0RXD): Der CH340 kann über GPIO 3 mitversorgt werden, wenn er als Ausgang gesetzt ist (Hinweis aus dem FAQ des Herstellers).
- GPIO 12, 0, 2, 5, 15: Strapping-Pins, Start kann gestört werden.
- GPIO 34 bis 39: nur Eingang.

## Ungeklärt

- Polarität der Relais: „HIGH = eingeschaltet“ ist eine Annahme aus dem Beispiel.
- Ob die Audio-Ausgabe über den 40-poligen Header oder die UEXT-Leiste geführt wird, ist nicht entschieden.
