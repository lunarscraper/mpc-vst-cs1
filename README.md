# mpc-vst-cs1

**CS-1**: CS-80-artiger Synthesizer als VST2-Plugin für die Akai Force (und andere MPC-OS-Geräte). Das Plugin ist ein Port der Firmware [CS-1](https://github.com/twochannels/CS-1) von *twochannels* für den M-VAVE FM-1, Stand 0.3.4.

**Status:** Version 0.1.0 ist fertig. Der Offline-Test auf x86 läuft (ASan/UBSan, CPU-Messung, Skin-Vorschau). Actions-Build für armhf und Test auf der Force stehen noch aus.

## Was es ist

Ein Port, kein Nachbau. Der Klang kommt aus dem Code der Firmware selbst (`vst/cs1/`). Dieses Repo legt nur die Plugin-Hülle darum.

- **Stimmen:** 8 Stimmen mit je zwei Kanälen, I und II. Wie beim CS-80 hat jeder Kanal einen eigenen kompletten Satz: VCO (Säge und/oder Puls mit PWM, Rauschen), Hochpass und Tiefpass in Reihe (je 12 dB mit Resonanz), Sinus nach den Filtern, Filterhüllkurve (IL, AL, A, D, R), VCA-Hüllkurve (A, D, S, R) und Touch Response (Velocity und Aftertouch auf Brillanz und Pegel).
- **Für alle Stimmen gemeinsam:** Sub-Oszillator (auf Tonhöhe, Filter und Pegel), Ring-Modulator, Brilliance und Resonance, Detune von Kanal II, Portamento und Glissando, Initial Pitch Bend, Keyboard Control und Unison. Dazu **Auto After Touch**: Jede Note spielt selbst eine Druckkurve, ein Sequencer hat ja keine Hände.
- **Effekte:** Drive mit Bass- und Höhen-EQ, Chorus/Tremolo/Flanger, Tape-Delay (synchron zum Tempo), Phaser und eine Dattorro-Plate mit Shimmer.
- **Klänge:** 69 Werksklänge: Vangelis, Toto/Thriller, 80er, die Beispiele aus dem CS-80-Handbuch, Paare und Klänge nach CS-80-V-Presets. Dazu 32 eigene Speicherplätze und die RANDOM-Funktion der Firmware (aus 910 Klängen gelernt) mit UNDO.

## Aufbau

```
MIDI ─► cs1_vst.cpp (Parameter, Programme, MIDI, Tempo, Chunk) ─► cs1_engine.c ─► cs1/src/cs1.c (Firmware, unverändert)
                                                                   (8 Kopien)       cs1_voice.h, cs1_fx.h, cs1_dsp.h
```

- **Mehrere Instanzen:** Alle Variablen der Firmware sind `static`. Deshalb wird `cs1_engine.c` achtmal übersetzt (`-DCS1_SLOT=0..7`), und jede Kopie ist ein eigener, vollständiger Synth. Bis zu **8 CS-1 gleichzeitig**, so viele wie die Force Plugins zulässt. Eine neunte Instanz bleibt stumm und stürzt nicht ab.
- **Plattform:** `cs1/src/platform.c` ersetzt die Hardware durch das „Fake-FM-1“ der Firmware (`fake_fm1.h`, aus `tests/` der Firmware). Die Hauptschleife der Firmware (Panel, Bildschirm, Flash) läuft nicht. Das Plugin ruft die Noten-, Druck-, Sustain- und Random-Funktionen der App direkt auf und schreibt das Panel (den Patch) direkt.
- **Blockgröße 32 statt 256 Samples:** Eine Note startet höchstens eine halbe Blocklänge (0,36 ms) neben ihrem Zeitpunkt (im Test gemessen). Fünf Stellen in `cs1_voice.h` liefen einmal pro 256er-Block. Sie sind auf die kleinere Blockgröße umgerechnet und mit `mpc-vst-cs1` markiert: Initial Pitch Bend, Ribbon, Auto-AT-Anstieg und -Abfall, Vintage-Drift. Gegenprobe: Ohne Chorus, Hall, Delay und Vintage klingt der Klang bei Block 32 und 256 sample-gleich, abgesehen von Klängen mit Portamento (Rundung der Gleitschritte).
- **Samplerate:** Die Firmware rechnet fest mit 44,1 kHz, wie die Force. Bei einer anderen Rate (z. B. 48 kHz am PC) wird linear umgerechnet, die Stimmung bleibt richtig.
- **CPU:** 8 Stimmen mit Chorus, Delay und Plate brauchen 1,2 % eines x86-Kerns (2,1 GHz). Die Firmware rechnet nur mit Ganzzahlen, sie ist für 240 MHz gebaut.

## Bedienung: die 10 Seiten

Auf den Klangseiten ist die **obere Reihe Kanal I**, die **untere Kanal II**, wie die zwei Tonreihen am CS-80. Die Q-Links übernehmen die Regler einer Seite Reihe für Reihe (erst I, dann II). Tasten liegen nicht auf den Q-Links.

| Seite | Obere Reihe | Untere Reihe |
|---|---|---|
| TONES | Tone I, Level I, Brilliance, Resonance, Mix I/II | Tone II, Level II, Detune II, Portamento, Volume |
| VCO | Feet, Wave, PW, PWM, PWM Speed, Noise (I) | dasselbe für II |
| VCF | HPF, HPF Res, LPF, LPF Res, VCF Level, Sine (I) | dasselbe für II |
| VCF EG | IL, AL, Attack, Decay, Release (I) | dasselbe für II |
| VCA | Attack, Decay, Sustain, Release, Init Bril, Init Lev, After Bril, After Lev (I) | dasselbe für II |
| MOD | Sub Func, Sub Speed, Sub > VCO, Sub > VCF, Sub > VCA, Sub Sync | Init P.Bend, After Speed, After VCO, After VCF, Gliss, Sus II |
| PERFORM | Auto AT an/aus, AT Delay, AT Rise, AT Level, AT Fall, Vintage, Unison, Velocity | Key Bril Lo/Hi, Key Lev Lo/Hi, Tune, Transpose, Tone |
| FX 1 | Ring Mod, Speed, Depth, Attack, Decay, Drive, Drive Level | Chorus (Off/Chorus/Tremolo/Flange), Speed, Depth, Phaser Rate, Depth, Feedback, Mix |
| FX 2 | Delay Time, Feedback, Mix, Sync, Ping, Damp, Tape | Reverb Mix, Size, Damp, Shimmer, Shim Shift, EQ Low, EQ High |
| PRESET | LOAD, Preset-Regler, SAVE | ROLL, UNDO, Amount, Scope, Category |

- **Tone I / Tone II** sind die Tontasten des CS-80: 58 Töne, die 22 aus dem CS-80-Handbuch plus die umgerechneten. Ein neuer Ton lädt alle Kanalwerte seiner Reihe, nur Feet und Level bleiben (wie in der Firmware). Die Reihe lässt sich danach frei verändern.
- **Tune, Transpose, Tone (Höhenabsenkung wie am Ausgang des CS-80), Velocity-Kurve, Sustain II und Auto AT an/aus** sind Einstellungen des Projekts. Sie stehen im Projekt, aber nicht im Preset, wie die MAIN-, MODES- und SEQ-Einstellungen der Firmware.
- **Werte** stehen wie in der Firmware von 0 bis 127. Werte mit Mittelstellung zeigen ± (64 = 0), PW zeigt die Pulsbreite in %, Tune Cent, Transpose Halbtöne.

## Klänge und Speicherplätze

- **Programme 1–69:** die Werksklänge der Firmware. **70–101:** die eigenen Plätze U01–U32. Alle erscheinen in der Preset-Liste der Force.
- Der **Preset-Regler** blättert nur. **LOAD** lädt den angezeigten Eintrag, **SAVE** speichert in den angezeigten U-Platz. Steht der Regler auf einem Werksklang, speichert SAVE in den ersten freien U-Platz, und der Regler springt dorthin. Ein U-Platz trägt den Namen des Klangs, aus dem er entstand.
- **Datei:** `cs1_presets.txt` neben dem Plugin-Ordner, gemeinsam für alle Instanzen und Projekte (Muster von Acid und Rattler). Ein Preset enthält den Klang, also beide Reihen, die globalen Werte und die Töne.
- **ROLL** ist die RANDOM-Seite der Firmware: ein Mittelwert je Kategorie plus Zufall entlang der zwölf Richtungen, in denen sich 910 Klänge unterscheiden. Danach greifen die Regeln der Firmware (keine Effekt-Stapel, hörbare Filter). **Amount** gibt an, wie weit gewürfelt wird. **Scope** wählt ALL, TONES (nur die Kanäle), GLOBAL oder FX. **Category** gibt die Richtung vor (PAD, STRINGS, BRASS …).
- **UNDO** nimmt ROLL und LOAD zurück, bis zu 16 Schritte.

## MIDI

| Eingang | Wirkung |
|---|---|
| Note, Velocity | durch die Velocity-Kurve, 8 Stimmen, Sustain II beendet beim nächsten Akkord den gehaltenen |
| Pitch Bend | ± 2 Halbtöne |
| Channel- und Poly-Aftertouch | Druck („After“ der Touch Response), dazu Auto AT |
| CC 1 | als Druck, wie die Expression-Einstellung der Firmware |
| CC 64 | Sustain |
| CC 5 / 7 | Portamento / Volume |
| CC 71, 85 / 74, 84 | Resonance / Brilliance |
| CC 72 / 73 / 75 | VCA Release / Attack / Decay, beide Reihen |
| CC 76 / 77, 86 | Sub Speed / Sub > VCO (Vibrato) |
| CC 78 | Auto AT Level |
| CC 91, 87 / 93 / 94 / 95 | Reverb Mix / Chorus Depth / Delay Mix / Shimmer |
| CC 20 / 21 | ROLL / UNDO, sobald der Wert die Mitte nach oben überschreitet |
| CC 120, 123 | alle Noten aus |
| Program Change | Programm 1–101 |
| Tempo und Transport der Force | Delay Sync und Sub Sync; Start setzt den Sub-Oszillator zurück (wie MIDI Start) |

Die CC-Belegung ist die der Firmware. Damit passt sie zu den Macros des Circuit Tracks, für die sie gedacht ist.

## Was vom FM-1 fehlt

- Bildschirm, Seitenknöpfe, Ribbon (SELECT-Encoder) und MIDI-Learn der Firmware: Auf der Force übernehmen Skin, Q-Links und Parameterliste ihre Aufgaben.
- CC 80–83, die seitenabhängigen Regler der Firmware, entfallen. Die übrige CC-Belegung bleibt.
- Die Tastatur-Sensorik (Velo) und USB-Audio des FM-1.

## Bauen und testen

GitHub → Actions → „VST release (draft)“ → Run workflow. Als Version z. B. `0.1.0` eintragen, `dry_run` an für einen reinen Build. Der Workflow baut `cs1.so` für armhf, erzeugt den Skin und lässt `vst/test.sh` laufen (Host-Test mit ASan/UBSan plus CPU-Messung).

Lokal ohne Docker:

```sh
MPC_VST=../mpc-vst-plugins CS1_SKIN_ONLY=1 bash vst/build.sh   # params.h + Skin
CS1_NO_DOCKER=1 bash vst/test.sh                                # Host-Test + Benchmark
```

`vst/tools/gen_cs1.py` erzeugt `module.json`, `layout.conf` und `cs1_keys.h` aus einer Tabelle. Parameter dort nur **anhängen**, nie umsortieren: Automation und Q-Links gehen nach Index.

## Dateien

| | |
|---|---|
| `vst/cs1_vst.cpp` | das Plugin (handgeschriebene VST2-Hülle nach Vorlage mpc-vst-rattler) |
| `vst/cs1_engine.c`, `vst/cs1_api.h` | eine Engine pro Slot: die App der Firmware plus die Funktionen fürs Plugin |
| `vst/cs1/src`, `vst/cs1/gen` | die Firmware (twochannels, CS-1 0.3.4), übernommen. Geändert sind nur `cs1_voice.h` (Blockgröße, markiert) und `fake_fm1.h` (HALF_FRAMES per `#ifndef`). `platform.c` ist ein eigener Ersatz. |
| `vst/host_test.cpp`, `vst/test.sh` | Offline-Test und Benchmark |
| `vst/tools/gen_cs1.py` | Parameter- und Skin-Tabelle |

## Lizenz und Herkunft

GPL-3.0-only (`LICENSE`), wie die Firmware. Wer die Plugin-Datei weitergibt, gibt den Quellcode mit.

- **CS-1** (App, Stimme, Effekte, Sounds): twochannels
- **Plattform-Schicht und Fake-FM-1:** Dinghy (zvenson) auf Felucca/SLOOP (Leo Kuroshita, Hügelton Instruments; isod89)
- **Tastensensorik `sense.c`:** Charles Vestal
- **Font:** Terminus (SIL OFL 1.1) für den nicht benutzten Bildschirm, DejaVu Sans Bold für die Skin-Beschriftung (`vst/fonts/`)

Einzelheiten in `vst/cs1/LICENSING-upstream.md`. „CS-80“ ist Yamaha, „FM-1“ M-VAVE. Kein Bezug zu einem von beiden.
