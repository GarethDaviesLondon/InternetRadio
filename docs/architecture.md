# Architecture

## Source layout

The whole sketch lives in `winRadio/`. Each feature is a `module.{h,cpp}`
pair so the dependency graph stays explicit; `winRadio.ino` only does
`setup()` / `loop()` orchestration.

```
winRadio/
  winRadio.ino       setup() + loop() orchestrator only

  config.h           pin map, audio constants, DISPLAY_ROTATION, hostnames
  branding.cpp       shared CSS + SVG used by every web page

  storage.{h,cpp}    NVS (Preferences) accessors, WiFi credential list,
                     SD card mount + read/write helpers
  net.{h,cpp}        WiFi STA scan / connect / reconnect / RSSI;
                     captive-portal probe (netCheckCaptive)
  provision.{h,cpp}  SoftAP + DNS captive portal for first-time setup
                     and as a fallback when the STA join fails or hits
                     a captive portal

  radio_audio.{h,cpp}  ES8311 codec init, ESP32-audioI2S setup,
                       station playback, ICY metadata callbacks,
                       Morse "CIT" boot self-test
  stations.{h,cpp}     Variable-length saved station list (0..30),
                       stored in NVS; SD CSV / compiled defaults seed
                       on first boot
  stations_defaults.h  Compile-time seed list (one file to edit for a
                       custom factory image)

  display.{h,cpp}    ST7789 panel + LovyanGFX off-screen sprite,
                     all draw routines, theme + layout, modal screens
                     (sys-info, station picker, wifi picker, captive)
  input.{h,cpp}      3 buttons -> short / double / long events
  imu.{h,cpp}        QMI8658 6-axis driver; emits motion + orientation
                     edge events for face-down pause + screen wake
  power.{h,cpp}      Battery sample + ext0 deep-sleep wake
  clock.{h,cpp}      SNTP + POSIX-TZ formatting (banner + LCD clock)
  log.{h,cpp}        Serial / SD log routing

  web.{h,cpp}        Home + /discover + /wifi + /api-docs pages,
                     all /api/* endpoints
  discover.{h,cpp}   Radio-Browser API client (used by /discover and
                     the CLI `find` command)
  cli.{h,cpp}        USB-CDC serial CLI

  es8311.{cpp,h}     Vendor codec driver
  es8311_reg.h       Codec register defs
  NotoSansBold15.h   UI font (LovyanGFX)
  touch.cpp          Touch driver stub for future variants
```

## Dependency direction

```
                ┌───────────────┐
                │  winRadio.ino │  (setup + loop)
                └───────┬───────┘
                        │
        ┌───────────┬───┼────────┬────────────┐
        ▼           ▼   ▼        ▼            ▼
       cli         web  display  imu        provision
        │           │    │
        ▼           ▼    ▼
    radio_audio   net   power
        │           │
        ▼           ▼
    stations    storage
        │           ▲
        ▼           │
    stations_defaults
```

No circular dependencies. Adding a feature usually means a new module
pair plus a wiring line in `winRadio.ino`.

## Threading

ESP32-S3 has two cores; FreeRTOS schedules tasks across them.

| Task                         | Core | Notes |
| ---------------------------- | ---- | ----- |
| Arduino `loopTask`           | 1    | runs `setup()` / `loop()` |
| ESP32-audioI2S audio task    | 1    | spawned by `Audio::begin()` |
| WiFi + lwIP                  | 0    | ESP-IDF default |
| Bluetooth (unused)           | 0    | not initialised |

The home-network web server (`WebServer`) is single-threaded and runs
inside `loop()` via `webPoll()` -> `s_http.handleClient()`. So a long
HTTP request stalls audio briefly. The captive-portal-detection probe
also runs inline; both are bounded to a few seconds.

The 115 KB LovyanGFX sprite is allocated in PSRAM
(`s_sprite.setPsram(true)`) so the audio library can claim contiguous
DMA-capable internal SRAM at boot.

## NVS schema (Preferences)

The ESP32 NVS partition holds all persistent state. Each "namespace"
is a logical group; keys are short to fit the 15-char limit.

| Namespace | Key  | Type   | Meaning |
| --------- | ---- | ------ | ------- |
| `wifi`    | `n`  | int    | Number of saved networks (0..10) |
| `wifi`    | `s0`..`s9` | string | SSID for slot i |
| `wifi`    | `p0`..`p9` | string | Password for slot i |
| `radio`   | `vol`  | int  | Last volume (raw 0..21) |
| `radio`   | `sta`  | int  | Last station slot |
| `radio`   | `bass` / `mid` / `trb` | int8 | Last EQ |
| `stations`| `cnt`  | int  | Active station count (0..30) |
| `stations`| `na0`..`na29` | string | Friendly name for slot i |
| `stations`| `ur0`..`ur29` | string | Stream URL for slot i |
| `clock`   | `tz`   | string | POSIX TZ string |

NVS is wiped by `pio run -t erase` or by the CLI commands
`wifi clear`, `station reset-all`. Reflashing alone does NOT erase NVS.

## Display modes

`enum DisplayMode` in `display.h`:

| Mode                  | When entered |
| --------------------- | ------------ |
| `DM_NOW_PLAYING`      | default home; banner + clock + now-playing card |
| `DM_BIG_CLOCK`        | left short-press from NP; full-screen 7-seg time |
| `DM_SYS_INFO`         | long-press Right; battery + SSID + RSSI + IP + host |
| `DM_PICKER`           | long-press Mid; 3x3 grid of station cards |
| `DM_WIFI_PICKER`      | long-press Right while in sys-info; 3x3 grid of scanned SSIDs |
| `DM_WIFI_CONNECT`     | shown during a join attempt; growing-dots progress |
| `DM_STATION_DETAIL`   | left double-click from NP; URL + ICY + bitrate + song |
| `DM_CAPTIVE`          | post-join probe sees a captive portal |

Modal modes (`DM_SYS_INFO` and below) remember the prior mode and
restore it via `displayModalClose()`.

## Boot sequence

`winRadio.ino:setup()`:

1. `Serial.begin(9600)` (USB-CDC)
2. `cliBegin()` -- prompt + welcome
3. `wifiLoadNetworks()` -- pull saved creds from NVS
4. `Wire.begin()` -- I2C for ES8311 + IMU
5. `inputBegin()`, `powerBegin()`, `imuBegin()`
6. `storageSdMount()` (best-effort), `stationsBegin()` (NVS or seed)
7. `audioCodecInit()` + `audioPlayMorseR()` ("CIT" CW boot chirp)
8. `audioRestoreSession()` -- volume / EQ / last-station slot
9. `displayBegin()` + theme load
10. Boot splash + 10 s minimum hold
11. `netBegin()` + `netScanNow()` + setup-button check
12. `provisionStartBackground()` (AP up alongside STA attempts)
13. `netConnect(...)` loop -- walks saved-network list; abort on
    Right-button hold OR portal-abort; falls into `runWifiSetup()`
    if list exhausts
14. On success: `provisionStop()`, `clockBegin()` (SNTP), `audioBegin()`,
    `audioStartLast()`, `webBegin()`
15. Captive probe -- if PORTAL: `displayCaptiveOpen()` and bring AP
    back up

## Loop responsibilities

`loop()` runs on Core 1 ~once every few ms:

- Slow tick (4 Hz): battery sample, repaint flag
- Captive probe (15 s) if `DM_CAPTIVE` is up
- Song-title scroll tick (33 Hz)
- Reboot combo check (Left + Right held 3 s)
- `inputPoll()` + mode-aware dispatch
- `imuLoop()` + face-down / motion event handling
- `cliPoll()`, `webPoll()`, `audioLoop()`
- Backlight dim tick + repaint if pending

## Web architecture

`web.cpp` uses the synchronous `WebServer` library (not Async). One
HTTP request runs to completion inside `webPoll()` before the next
tick, so connect-style endpoints (`/api/wifi-connect`,
`/api/wifi-connect-adhoc`) intentionally block for up to 12 s while
the radio attempts the join.

mDNS announces the device as `WebRadio.local` (configurable via
`DEFAULT_HOSTNAME` in `config.h`).

The provisioning AP runs a separate `WebServer` instance in
`provision.cpp` on the same port 80 -- only ever active when the
SoftAP is up, so the two servers don't conflict.

## Captive-portal handling

`netCheckCaptive()` in `net.cpp` does a raw HTTP/1.1 GET to
`connectivitycheck.gstatic.com/generate_204` (Android's check) with a
Microsoft fallback. We parse the status line + Location header
ourselves so a captive 30x is preserved -- HTTPClient's auto-follow
would mask it.

Triggered:
- once at boot, after the saved-list join succeeds
- after every CLI `wifi connect` and on-device WiFi-picker connect
- every 15 s from `loop()` while `DM_CAPTIVE` is shown

When `CAPTIVE_PORTAL` is reported, the SoftAP is brought back up so
the user has a control surface (abort, switch SSID, reorder) while
they complete the login on a phone. When the periodic re-probe sees
`CAPTIVE_ONLINE`, the AP is torn down again and audio resumes.

## Build variants

Defined in `platformio.ini`:

| Env                               | Purpose |
| --------------------------------- | ------- |
| `waveshare-s3-radio` (default)    | Stock build |
| `waveshare-s3-radio-rot90`        | Panel rotated 90° clockwise (`-DDISPLAY_ROTATION=1`) |

See [hardware.md](hardware.md) for the full pin map and variant
notes.
