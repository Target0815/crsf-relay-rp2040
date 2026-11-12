[English](README.md) | **Deutsch**

# RC-Relay RP2040

Ein Relais für Fernsteuerungen mit CRSF: Es verbindet zwei Funkstrecken hintereinander und
verlängert so die Reichweite oder umgeht Abschattungen – den **Sender-Link** (Sender → Relais)
und den **Modell-Link** (Relais → Modell).

![Einsatz des Relais](docs/relay-setup.de.svg)

Das Relais steht dort, wo es freie Sicht zum Modell hat – auf einem Stativ, auf dem
Autodach oder einem Aussichtsturm. Der Sender bleibt beim Piloten, zum Beispiel im Auto.
Geeignet für Flugmodelle, RC-Cars und Boote.

> **Benutzung auf eigene Gefahr.** Das Failsafe-Verhalten vor jedem Einsatz am Boden testen
> (siehe [Failsafe](#failsafe)).

## Was das Relais kann

* **Modell-Link mit ELRS oder mLRS** – das Relais erkennt den Modultyp selbst.
* **Alles über das ELRS-Lua-Skript einstellen:** das Relais selbst, das Sendemodul des Relais und
  den Empfänger im Modell. Bei mLRS zeigt das Relais auch die mLRS-Parameter im ELRS-Lua an.
* **Telemetrie** vom Modell (GPS, Akku, Flugmodus …) kommt wie gewohnt am Sender an,
  die Werte beider Funkstrecken bleiben getrennt.
* **Failsafe:** Fällt der Sender-Link aus (z. B. Sender aus), geht das Modell in sein Failsafe.
* **Schnelles Lua-Menü:** verlorene Anfragen wiederholt das Relais selbst.
* Halbduplex (Modulschacht/JR-Pin) und Vollduplex, Watchdog, USB-Konsole zur Diagnose.

## Was man braucht

* einen **Sender mit ELRS-Sendemodul** (intern oder extern) für den Sender-Link, z. B. RadioMaster TX15
* **Waveshare RP2040-Zero** (oder ein anderes RP2040-Board, Pins siehe unten)
* einen **CRSF-Empfänger** für den Sender-Link, z. B. RadioMaster ER3C-i (ELRS)
* ein **Sendemodul** für den Modell-Link: ELRS-Modul (z. B. RadioMaster Bandit Nano) oder mLRS-TX
  (z. B. MatekSys mR900-30, ab mLRS v1.4.04)

## Firmware aufspielen

1. Die UF2-Datei unter **Releases** herunterladen (`crsf-relay-<version>-rp2040-zero.uf2`).
2. RP2040-Zero mit gedrückter **BOOT**-Taste an USB anstecken, es erscheint das Laufwerk `RPI-RP2`.
3. Die UF2-Datei darauf kopieren. Einstellungen bleiben bei einem Update erhalten.

## Anschluss

| RP2040-Zero | geht an                                                       |
|-------------|---------------------------------------------------------------|
| GP0         | RX des Empfängers (Sender-Link)                               |
| GP1         | TX des Empfängers (Sender-Link)                               |
| GP4         | Sendemodul: **Halbduplex** – die eine Signalleitung (JR-Pin 5 / „In“); **Vollduplex** – RX des Moduls |
| GP5         | Sendemodul: nur Vollduplex – TX des Moduls                    |
| 5V, GND     | Versorgung 5 V, gemeinsame Masse aller Geräte                 |

Alle Signale haben 3,3-V-Pegel. Die Polarität der Modulseite stellt man im Menü ein
(„Inverted“, siehe Tabelle).

> **Hinweis:** Mit echter Hardware getestet ist bisher nur der **Halbduplex**-Betrieb
> (ELRS: RadioMaster Bandit Nano, mLRS: MatekSys mR900-30). Vollduplex ist umgesetzt, aber nur
> ohne Hardware geprüft.

> **Achtung USB:** Beim RP2040-Zero ist 5V direkt mit der USB-Buchse verbunden. Vor dem Anstecken
> an den PC die externe 5-V-Leitung abklemmen (GND verbunden lassen) oder einen USB-Power-Blocker
> verwenden.

## Einstellungen auf einen Blick

Im ELRS-Lua-Skript das Gerät **RC-Relay RP2040** wählen.

| Menüpunkt     | mit ELRS-Modul                                   | mit mLRS-Modul             | Bedeutung |
|---------------|--------------------------------------------------|----------------------------|-----------|
| Half-Duplex   | **On** (Modulschacht) / Off (Empfänger als Sender) | **On**                   | eine Signalleitung (GP4) oder zwei (GP4/GP5) |
| Baudrate      | 921k (Standard)                                  | **400k**                   | Geschwindigkeit zum Sendemodul |
| Inverted      | **Off**                                          | **On**                     | Polarität der Modulseite |
| LinkStat      | Transform                                        | Transform                  | wie die Werte des Modell-Links am Sender ankommen |
| Telemetry     | Forward                                          | Forward                    | Telemetrie vom Modell an den Sender |
| Forward Rate  | 2                                                | 2                          | jede n-te Telemetriemeldung weitergeben (entlastet den Sender-Link) |
| ArduPilot     | Forward (bei ArduPilot-FC)                       | egal                       | ArduPilot-Telemetrie (Yaapu-Widget) |
| Own Telemetry | nach Wunsch                                      | nach Wunsch                | Chiptemperatur des Relais als Sensor |

**Fett** = vom Standard abweichend bzw. wichtig.

Am **mLRS-Modul** und **mLRS-Empfänger** (über das Relais im ELRS-Lua oder am Modul-Display):

| mLRS-Parameter      | Wert                       |
|---------------------|----------------------------|
| Tx Ch Source        | crsf (Standard)            |
| Rx Out Mode         | crsf (an den Flugcontroller) |
| Rx Failsafe Mode    | nach Wunsch, z. B. „no signal“ |
| Config-ID + 1       | **nicht** mit gleicher Bind Phrase an dieses Modell binden (siehe Failsafe) |

## Inbetriebnahme

1. **Erst den Sender einschalten, dann das Relais.** (Ein mLRS-Modul sucht seine
   Einstellungen nur in den ersten Sekunden nach dem Einschalten.)
2. Die LED am RP2040-Zero zeigt den Zustand:

   | LED                     | Zustand |
   |-------------------------|---------|
   | blau, dauerhaft         | Start, wartet auf den Empfänger |
   | grün, 2× blinken        | läuft (Halbduplex: cyan) |
   | rot, 1× kurz            | keine Daten vom Empfänger (Sender-Link weg) |

3. Im ELRS-Lua erscheinen: das Sendemodul des Senders, der Empfänger des Relais,
   **RC-Relay RP2040**, das Sendemodul des Relais und der Empfänger im Modell (bei ELRS).

## Das Menü des Relais

| Eintrag        | Bedeutung |
|----------------|-----------|
| Firmware       | Version der Relais-Firmware |
| Watchdog       | Anzahl Neustarts durch den Watchdog (sollte 0 bleiben) |
| Total Retries  | wiederholte Menü-Anfragen seit dem Einschalten (steigt bei schwacher Verbindung) |
| Chip Temp.     | Temperatur des RP2040 |
| übrige Einträge | siehe [Einstellungen auf einen Blick](#einstellungen-auf-einen-blick) |

**LinkStat** im Detail: *Transform* zeigt die Werte des Modell-Links als eigene Sensoren (RRSP, TRSP,
RPWR, TFPS), ohne die Werte des Sender-Links zu stören. *Forward* gibt sie unverändert weiter (dann
springen die Sensoren zwischen beiden Strecken), *Tunnel* verpackt sie für Lua-Skripte/Widgets,
*Off* verwirft sie.

## mLRS: Parameter im ELRS-Lua

Das mLRS-Modul erscheint in der Geräteliste (z. B. **Matek mR900-30**). Das Relais lädt die
mLRS-Parameter nach dem Einschalten im Hintergrund und zeigt sie dort an:

* oben: TX und RX mit Name und Version, darunter **Save**
* Ordner **General**, **Tx**, **Rx** und **Rx Failsafe**

Änderungen wirken – wie im mLRS-Lua – erst nach **Save**. Ordner mit noch nicht gespeicherten
Änderungen zeigen „(n unsaved)“ im Namen. Nach dem Speichern das Lua einmal schließen und neu
öffnen, dann sind die Markierungen weg.

Hinweise:
* Steht dort „loading …, reopen“, lädt das Relais noch: Lua schließen und nach ein paar Sekunden
  neu öffnen.
* Rx-Parameter erscheinen nur, wenn der mLRS-Empfänger im Modell verbunden ist.
* Die Bind Phrase ist nur lesbar (das ELRS-Lua kann keinen Text bearbeiten).

## Failsafe

* **ELRS:** Fällt der Sender-Link aus, hört das Relais auf zu senden. Das ELRS-Modul stellt die Verbindung
  zum Modell ein, der Empfänger im Modell geht in sein Failsafe.
* **mLRS:** Ein mLRS-Modul würde ohne Eingangssignal einfach die letzten Werte weitersenden.
  Das Relais schaltet deshalb bei Ausfall des Sender-Links auf eine andere mLRS-Konfiguration um
  (Config-ID + 1, wie das Ändern der „Receiver ID“ in EdgeTX). Die Verbindung zum Modell reißt
  ab, der mLRS-Empfänger geht in sein Failsafe. Kommt der Sender-Link zurück, schaltet das Relais zurück,
  nach einigen Sekunden steht die Verbindung wieder. Aussetzer unter 0,75 s bewirken nichts.
  Wichtig: Die Konfiguration Config-ID + 1 darf nicht mit derselben Bind Phrase gebunden sein
  (Standard „mlrs.0“, „mlrs.1“ … ist in Ordnung).
* **Relais stromlos:** Wird das Sendemodul des Relais aus derselben Quelle versorgt, ist es dann
  ebenfalls aus, und das Modell geht in sein Failsafe.

**Test:** Modell sicher am Boden, Sender ausschalten → Flugcontroller bzw. Modell muss Failsafe
melden. Sender wieder ein → Verbindung muss zurückkommen.

## Fehlersuche

| Problem | Lösung |
|---------|--------|
| mLRS: keine Steuerung, keine Telemetrie | Baudrate 400k und Inverted On prüfen; erst Sender, dann Relais einschalten |
| mLRS mit 921k geht nicht | bei 400k bleiben – reicht für alle mLRS-Modi |
| Relais-Menü lädt langsam | normal beim ersten Öffnen; „Total Retries“ zeigt Wiederholungen |
| Sensoren springen zwischen zwei Werten | LinkStat auf Transform stellen |
| LED rot | Empfänger des Relais hat keine Verbindung zum Sender bzw. keine Kanäle |

## USB-Konsole (für Diagnose)

RP2040-Zero per USB an den PC (Warnung oben beachten), beliebiges Terminalprogramm:
`s` Status · `p` Protokoll der Menü-Anfragen an/aus · `b` Bootloader (Update) · `r` Neustart ·
`d` Werkseinstellungen.

## Herkunft und Lizenz

Basiert auf dem **RC-Relay32** von Wilhelm Meier
([wmucpp](https://github.com/wimalopaan/wmucpp/tree/master/boards/rcRelay32)), portiert auf den
RP2040 und erweitert (u. a. mLRS). Lizenz: GPL-3.0-or-later.

Fragen, Probleme oder Ideen bitte als [Issue](../../issues) auf GitHub.
