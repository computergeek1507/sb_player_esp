# ESP32-P4 SB Player

An FSEQ sequence player for the ESP32-P4 that drives holiday and animated light displays. Reads `.fseq` v2 files from an SD card and streams channel data out over E1.31 (sACN) and/or DDP to LED controllers, with synchronized audio playback and a time-based scheduler.

## Features

- **FSEQ v2 playback** — reads uncompressed sequences from SD card, frame-accurate timing via FreeRTOS tasks
- **E1.31 (sACN) output** — up to 8 outputs, unicast or multicast, configurable universe/channel mapping
- **DDP output** — up to 8 outputs with per-output channel mapping
- **Audio playback** — WAV and MP3 via I2S DAC (PCM5102A / UDA1334A), auto-matched from sequence media field
- **MultiSync** — master mode to keep secondary players frame-synced
- **Scheduler** — time-windowed autoplay with named playlists; repeat and shuffle support
- **Wired Ethernet** — DHCP or static IP; NTP time sync
- **WiFi AP** — always-on access point for configuration (`SB-Player` / `sbplayer` by default)
- **Web UI** — browser-based management for config, outputs, sequences, playlists, schedule, and OTA firmware updates
- **OLED status display** — 128×64 SSD1306 shows IP, current sequence, frame progress, FPS, and audio state
- **RTC support** — DS3231, DS1307, or PCF8523 for accurate scheduling without NTP; NTP writes back to RTC on first sync

## Repository Layout

```
firmware/SBPlayer/       Arduino sketch and headers
  SBPlayer.ino           Main sketch: setup, playback task, audio task, loop
  config.h               Device config struct + JSON load/save
  fseq.h                 FSEQ v2 parser
  audio.h                I2S audio player (WAV/MP3)
  e131.h                 E1.31/sACN sender
  ddp.h                  DDP sender
  multisync.h            FPP MultiSync master
  scheduler.h            Time-based scheduler + playlist engine
  oled.h                 SSD1306 OLED driver
  rtc.h                  RTC abstraction (DS3231/DS1307/PCF8523)
  webui.h                Async HTTP + WebSocket API and web UI
  data/                  LittleFS web pages (config, outputs, sequences,
                          playlists, schedule, OTA)

hardware/                KiCad ( PCB project (ESP32-P4 + OLED, I2S audio and RTC carrier)
  ESP32-P4_OLED.*        Schematic, PCB, project files
  jlcpcb/                JLCPCB-ready gerbers, BOM, and CPL

tools/                   SB Card Builder (prepares the SD card from xLights)
  sb_card_builder.py     Python / Tkinter version (GUI + command line)
  sb_card_builder_flutter/  Flutter version (Windows desktop and web)
  sb_card_builder_qt/    C++ / Qt 6 version (GUI + command line)
  icon/                  App icon and the script that generates it
```

## Hardware

The carrier board breaks out:

| Connector | Purpose |
|-----------|---------|
| J1 (2×13) | ESP32-P4 module |
| J2 (1×4)  | I2C OLED (SSD1306) |
| J3 (1×6)  | I2S DAC (UDA1334A pin-compatible) |
| J4 (1×6)  | I2S DAC (PCM5102A pin-compatible) |
| U3        | PCF8523T RTC with 32.768 kHz crystal |
| U2        | CR1220 RTC battery backup |

Production files for JLCPCB are in `hardware/jlcpcb/`.

## Firmware Setup

**Board**: `esp32:esp32:esp32p4` (Arduino ESP32 core 3.x)

**Required libraries** (install via Arduino Library Manager or clone into `~/Arduino/libraries`):
- ESPAsyncWebServer
- ArduinoJson
- ESP32-audioI2S (or equivalent — provides `AudioFileSourceSD`, `AudioGeneratorWAV/MP3`, `AudioOutputI2S`)
- Adafruit GFX Library
- Adafruit SSD1306

**Build and flash**:
1. Open `firmware/SBPlayer/SBPlayer.ino` in Arduino IDE
2. Select board `ESP32P4 Dev Module` and your port
3. Flash the sketch
4. Flash the LittleFS filesystem image (`littlefs.bin`) using the ESP32 LittleFS upload tool — this deploys the web UI pages

## SD Card Layout

```
/sequences/      FSEQ v2 sequence files + matching audio (*.wav / *.mp3)
/playlists/      JSON playlist files
schedule.json    Active schedule (created/managed via web UI)
```

Example `schedule.json`:
```json
{
  "entries": [
    { "days": 127, "start_hour": 18, "start_min": 0,
      "end_hour": 23, "end_min": 0, "playlist": "default.json" }
  ]
}
```

Example `playlists/default.json`:
```json
{ "repeat": true, "shuffle": false, "entries": ["show1.fseq", "show2.fseq"] }
```

## SB Card Builder

`tools/sb_card_builder.py` prepares the SD card from an xLights show folder (Python 3.9+, Tkinter):

```
python tools/sb_card_builder.py                                   # GUI
python tools/sb_card_builder.py --show <show folder> --out <SD>   # command line
```

- **Outputs** — reads `xlights_networks.xml` and writes `outputs.json` (E1.31 and DDP controllers). The player imports this file from the card on the next boot and renames it to `outputs.imported.json`.
- **Sequences** — converts xLights `.fseq` files (zstd / zlib compressed, sparse, v1 or v2) to the uncompressed FSEQ v2 the player reads, and copies the matching `.mp3` / `.wav` into `/sequences`.
- **Playlists / Schedule** — builds `playlists/*.json` and `schedule.json`.
- **SB Pixel 16 export** — writes sequences for an [SB Pixel 16](https://github.com/computergeek1507/sb_pixel_16) controller instead: pick it under *Export for* on the Sequences tab, or use `--pixel16 <controller name>` with `--show` and `--out`. Each file holds only that controller's channels, uncompressed, in `/sequences`; no audio, `outputs.json`, playlists or schedule are written, because that controller just loops the folder. For an E1.31 controller (or DDP without *Keep Channel Numbers*) the channels are renumbered from 1, matching the port start channels used for E1.31. For DDP with *Keep Channel Numbers* they keep their xLights numbers (stored as one sparse range). The controller buffers at most 262,144 channels per frame and plays the first 64 files.

zstd-compressed sequences (the xLights default) need `python -m pip install zstandard` on Python older than 3.14.

The same tool also exists in two other forms. All three write identical files.

**Flutter (Windows desktop and web)** — `tools/sb_card_builder_flutter`, needs the Flutter SDK:

```
cd tools/sb_card_builder_flutter
flutter run -d windows            # or: flutter build windows  -> build\windows\x64\runner\Release\
flutter run -d chrome             # or: flutter build web      -> build\web\
```

The web build reads and writes folders through the File System Access API, so it needs Chrome or Edge on a computer, served from `localhost` or https. The Flutter app uses the same dark color scheme as the player's web UI.

To host the web build on your own web server:

```
flutter build web --release --base-href /sbcard/   # path it will be served from; omit for the site root
```

Copy everything in `build\web\` to that path on the server. The server must use https, and should serve `.wasm` as `application/wasm`. Send `index.html`, `flutter_bootstrap.js` and `flutter_service_worker.js` with `Cache-Control: no-cache` so visitors get new versions after an upload.

**C++ (Qt 6 Widgets)** — `tools/sb_card_builder_qt`, needs Qt 6, CMake 3.21+ and a C++17 compiler; zstd, nlohmann/json and pugixml are downloaded during configure:

```
cd tools/sb_card_builder_qt
cmake -S . -B build -DCMAKE_PREFIX_PATH=<Qt kit, e.g. D:/Qt/6.8.3/msvc2022_64>
cmake --build build --config Release
build\Release\sb_card_builder.exe                               # GUI
build\Release\sb_card_cli.exe --show <show folder> --out <SD>   # command line
```

## Default Pin Assignments

| Function | GPIO |
|----------|------|
| I2S BCLK | 26 |
| I2S LRCLK | 25 |
| I2S DATA | 22 |
| OLED SDA / RTC SDA | 7 |
| OLED SCL / RTC SCL | 8 |
| Ethernet PHY addr | 1 |
| Ethernet MDC | 31 |
| Ethernet MDIO | 52 |
| Ethernet power | 51 |

All pins are configurable via the web UI.

## Web UI Endpoints

| Path | Description |
|------|-------------|
| `/` | Dashboard — playback status, play/stop |
| `/config.html` | Device settings (network, audio, RTC, hostname) |
| `/outputs.html` | E1.31 and DDP output configuration |
| `/sequences.html` | Browse and play sequences from SD |
| `/playlists.html` | Manage playlists |
| `/schedule.html` | Time-based schedule editor |
| `/ota.html` | OTA firmware update |
| `/api/status` | JSON playback status (also pushed via WebSocket `/ws`) |

## License

[CC BY-NC 4.0](LICENSE) — free for personal and non-commercial use; commercial use is not permitted.

## First Boot

1. Connect to the `SB-Player` WiFi AP (password `sbplayer`)
2. Open `http://192.168.4.1` in a browser
3. Configure Ethernet, NTP timezone, output universes, and I2S pins
4. Copy sequences to the SD card and create a playlist
5. Add a schedule entry or play sequences manually from the UI
