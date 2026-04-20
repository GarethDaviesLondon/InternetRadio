# Waveshare Internet Radio

An ESP32-S3 internet radio for the Waveshare ESP32-S3 development board with
onboard 1.3" 240×240 ST7789 LCD and ES8311 audio codec.

Based on [VolosR/WaveshareRadioStream](https://github.com/VolosR/WaveshareRadioStream)
(see the project [video](https://www.youtube.com/watch?v=4ZUwnb6cSs4)).

## Hardware

- Waveshare ESP32-S3 dev board (same as Volos's video)
- ST7789 240×240 SPI LCD (onboard)
- ES8311 audio codec (onboard, I²C + I²S)
- 3 user buttons: GPIO 0 (left / sleep), GPIO 5 (mid / next station),
  GPIO 4 (right / volume)
- Li-ion battery monitoring on ADC1 channel (GPIO 1), with enable on GPIO 2

### Pin map

All pins live in `winRadio/config.h`:

| Function         | GPIO |
| ---------------- | ---- |
| I²C SDA          | 42   |
| I²C SCL          | 41   |
| I²S MCLK         | 8    |
| I²S BCLK         | 9    |
| I²S LRC          | 10   |
| I²S DOUT         | 12   |
| Amp enable (PA)  | 7    |
| LCD DC           | 45   |
| LCD CS           | 21   |
| LCD SCK          | 38   |
| LCD MOSI         | 39   |
| LCD RST          | 40   |
| LCD backlight    | 46   |
| Battery enable   | 2    |

## Build

### PlatformIO (recommended)

A `platformio.ini` ships in the repo, so a clone-and-build is enough:

```
git clone -b claude/waveshare-internet-radio-1syTE \
  https://github.com/GarethDaviesLondon/InternetRadio.git
cd InternetRadio
pio run -t upload
pio device monitor
```

Board: `esp32-s3-devkitc-1`, flash 16 MB, OPI PSRAM, serial 9600 8N1.
Libraries (`ESP32-audioI2S`, `Arduino_GFX`, `LovyanGFX`) are pulled
automatically via `lib_deps`.

### Arduino IDE

1. Install the **ESP32 Arduino core** (v3.x, tested with ESP32-S3).
2. Install these libraries via the Arduino Library Manager:
   - `ESP32-audioI2S` (schreibfaul1)
   - `Arduino_GFX` (moononournation)
   - `LovyanGFX`
3. Board: *ESP32S3 Dev Module*, PSRAM: *OPI PSRAM*, Flash size: *16 MB*,
   Partition: *Default 4MB with spiffs* (or any that leaves room for NVS),
   USB CDC On Boot: *Disabled* (so Serial stays on UART0 at 9600).
4. Open `winRadio/winRadio.ino` and upload.

## Serial CLI

The device exposes a serial command-line on **USB/UART0 at 9600 8N1**. WiFi
credentials are stored in NVS (the ESP32 `Preferences` namespace `wifi`), not
in source, so the sketch can be shared without leaking secrets.

### First boot

If no credentials are stored, the LCD shows a prompt and the device waits on
the serial port. Connect a terminal (screen / minicom / PuTTY / Arduino Serial
Monitor) at **9600 8N1** and type:

```
wifi
SSID: my-network
Password: ********
Saved to NVS. Type 'reboot' to apply.
> reboot
```

### Commands

| Command      | Action                                   |
| ------------ | ---------------------------------------- |
| `help`       | List commands                            |
| `wifi`       | Prompt for new SSID + password (masked)  |
| `wifi show`  | Print the stored SSID                    |
| `wifi clear` | Erase stored credentials from NVS        |
| `status`     | WiFi state, IP, RSSI, station, volume, battery |
| `reboot`     | Restart the device                       |

The CLI stays active while the radio is playing, so you can change networks or
check status without reflashing.

## Buttons (default firmware)

- **Left (GPIO 0)**: deep sleep
- **Mid (GPIO 5)**: next station
- **Right (GPIO 4)**: volume step (wraps 1–5)

## Architecture

The sketch is split into focused modules (each `module.{h,cpp}` pair) plus
`winRadio.ino` which only orchestrates `setup()` / `loop()`.

```
winRadio/
  winRadio.ino     setup() + loop() orchestrator only
  config.h         pin map + audio / display / power constants

  storage.{h,cpp}  NVS (Preferences) accessor + WiFi-creds convenience;
                   SD-card surface declared as stubs
  net.{h,cpp}      WiFi STA connect / reconnect / RSSI / hostname; mDNS
                   and LAN-broadcast declared as stubs
  radio_audio.{h,cpp}
                   ES8311 init, ESP32-audioI2S setup + callbacks, station
                   playback, Morse "R" speaker self-test. Named with a
                   prefix so the file doesn't collide with the library's
                   own `Audio.h` on case-insensitive filesystems.
  stations.{h,cpp} Preset list with names + URLs; future SD-loaded sets
  display.{h,cpp}  ST7789 panel + sprites + drawing; `Theme` struct so
                   skinning is one assignment away
  input.{h,cpp}    Buttons; touch-driver declared as stub
  power.{h,cpp}    Battery sampling + deep sleep
  cli.{h,cpp}      USB-CDC serial CLI; calls into the module APIs
  web.{h,cpp}      Web UI placeholder (see header for the planned design)
  provision.{h,cpp} AP / captive-portal placeholder for first-time WiFi

  es8311.{cpp,h}   ES8311 codec driver (vendor)
  es8311_reg.h     codec registers
  NotoSansBold15.h UI font
```

### Dependency direction

```
  cli, web ──────────────────┐
            │                │
            ▼                ▼
   audio  net  storage  power  input  display ── stations
                                         │           ▲
                                         └───────────┘  (read for rendering)
```

No circular dependencies. Adding a new feature usually means a new module
pair and one wiring line in `winRadio.ino`.

### Two-core strategy

- **Core 0**: WiFi + BT stacks (ESP-IDF default).
- **Core 1**: Arduino `loopTask` (this is where `setup()` / `loop()` run).
  ESP32-audioI2S spawns its own audio task on Core 1 by default; once the
  web server lands, call `audio.setAudioTaskCore(0)` to free Core 1's CPU
  budget for the display / loop tick.
- **Display**: a future enhancement is to move `displayDrawMain()` into a
  dedicated task on Core 0 alongside WiFi, so the heavy 115 KB sprite
  byte-swap stops blocking `audioLoop()` for ~30 ms every frame.

## Roadmap (stubbed in code)

- **Web UI** — `web.{h,cpp}`. Plan: ESPAsyncWebServer + AsyncWebSocket;
  static SPA from PROGMEM/LittleFS; JSON over WS for state push.
- **SD card** — `storage.{h,cpp}`. Settings / station sets / recordings /
  MP3 playback, all via `SD_MMC` on the existing pin map.
- **WiFi provisioning** — `provision.{h,cpp}`. AP `WaveRadio-Setup` +
  captive-portal DNS + a tiny scan/save form; falls through automatically
  when stored creds fail.
- **mDNS / discovery** — `netStartMdns()`. Announce as `waveradio.local`
  with a `_http._tcp` service record.
- **Skinnable UI** — `display.{h,cpp}` already routes everything through a
  `Theme` struct; load alternate themes from SD.
- **Touch** — `input.{h,cpp}`. Pick a driver (XPT2046 resistive or GT911
  capacitive depending on the board variant) and emit `INPUT_TOUCH_TAP`.

## Credits

- Original project and UI: [Volos Projects](https://github.com/VolosR/WaveshareRadioStream)
- ES8311 driver: Espressif (via Volos's port)
