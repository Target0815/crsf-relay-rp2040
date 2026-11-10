# ToDo – CRSF-Relay RP2040

* (derzeit nichts offen)

Regel für neue Dokumente: zweisprachig, mit Sprachkennung im Dateinamen
(z. B. `ANLEITUNG.de.md` / `ANLEITUNG.en.md`); die ToDo-Liste bleibt deutsch.

## Erledigt

* 1.0.1: Änderungen des Originals vom 8.11.2026 übernommen (DirectConnected → Run,
  ungültige Werte ablehnen, Relais startet ausgeschaltet)
* 1.0.2: Parameter-Trace in der USB-Konsole (`p`)
* 1.0.3: Relais wiederholt verlorene Parameteranfragen an Modul 2 / Empfänger 2
* 1.0.4: ArduPilot-Passthrough (0x80) als Telemetrie mit eigenem Menüpunkt
* 1.0.5: Wiederholung auch für den Flugcontroller (0xC8) und alle Geräte hinter dem Relais
* 1.0.6: Menüeintrag „RP2040 Firmware“ mit Version ganz oben im Relais-Menü
* Dokumentation zweisprachig: README.md englisch, README.de.md deutsch, Umschalter oben,
  Grafik als `docs/relay-setup.en.svg` / `.de.svg`
