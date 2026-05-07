# ON8CIT WebRadio

An ESP32-S3 internet radio firmware for the Waveshare ESP32-S3 dev
board (1.3" 240×240 ST7789, ES8311 codec, optional QMI8658 IMU + mics).

Originally derived from
[VolosR/WaveshareRadioStream](https://github.com/VolosR/WaveshareRadioStream),
substantially rewritten and extended.

## What it does

- **Streams** internet radio (MP3 / AAC / OGG / FLAC) from up to **30
  saved stations** via I²S to the ES8311 codec.
- **Discovers** new stations via the
  [Radio-Browser](https://www.radio-browser.info/) API (search by
  name / genre / country) -- from the web UI or the serial CLI.
- **Drives** itself with three buttons (short / double / long
  gestures), an IMU (face-down to pause, tilt to resume, motion to
  wake the screen), and a small set of on-device screens
  (now-playing, big clock, system info, station picker, WiFi picker,
  station detail, captive-portal helper).
- **Manages WiFi** through a SoftAP setup portal, an on-device WiFi
  picker, the home-network web `/wifi` page, and a serial CLI -- all
  three surfaces support add / connect / delete / reorder.
- **Detects captive portals** and surfaces the login URL on-screen so
  the user can authenticate from a phone, with automatic re-probe
  every 15 s and audio auto-resume when the portal clears.
- **Persists** everything (WiFi list, station list, EQ, last station,
  timezone) in NVS; survives reboots and reflashes (`pio run -t
  upload` does not erase NVS).

## Quick start

```
git clone https://github.com/GarethDaviesLondon/InternetRadio.git
cd InternetRadio
pio run -t upload
pio device monitor
```

After the first flash:

1. Connect a phone or laptop to the SoftAP `ON8CIT-WinRadio-Setup`.
2. Open `http://192.168.4.1` (most OSes auto-open it as a captive
   portal).
3. Pick a network from the scan list, enter the password, save.
4. The radio reboots, joins, and starts playing the first compiled-
   default station. Audio out goes through the onboard ES8311 +
   speaker.
5. From here, drive it via the three buttons, the web UI at
   `http://WebRadio.local`, or the serial CLI on the same USB-C port.

For everything else, see [`docs/`](docs/README.md).

## Documentation

| Doc                                  | Topic |
| ------------------------------------ | ----- |
| [docs/user-guide.md](docs/user-guide.md) | Buttons + screens + web pages + IMU |
| [docs/cli.md](docs/cli.md)               | Full serial CLI reference |
| [docs/api.md](docs/api.md)               | HTTP API (`/api/state`, `/api/station`, ...) |
| [docs/hardware.md](docs/hardware.md)     | Pin map, IMU detection, build variants |
| [docs/architecture.md](docs/architecture.md) | Module map, threading, NVS schema |
| [docs/building.md](docs/building.md)     | PlatformIO setup, ccache, troubleshooting |

## Highlights

- ~5 000 lines of C++, organised as one module per feature
  (`module.{h,cpp}` pairs)
- One file (`winRadio/stations_defaults.h`) to edit for a custom
  factory station list
- Build variants for different physical mountings via
  `DISPLAY_ROTATION` (e.g. `pio run -e waveshare-s3-radio-rot90`)
- Drag-drop reorder for the saved-station list and the saved-WiFi
  list (HTML5 native, no JS framework)
- Live HH:MM:SS clock in the web banner (server seed + 1 Hz JS tick)
- "Easy 90%" captive-portal detection: probe `generate_204`, parse
  Location, surface URL on-screen + retry every 15 s
- ccache + library-archive cache wired through `platformio.ini` for
  fast incremental builds

## Branch

Active development on
[`claude/waveshare-internet-radio-1syTE`](https://github.com/GarethDaviesLondon/InternetRadio/tree/claude/waveshare-internet-radio-1syTE).
The branch name is the development-session identifier; treat it as
the current trunk.

## Credits

- Original sketch + UI direction: [Volos
  Projects](https://github.com/VolosR/WaveshareRadioStream)
- ES8311 driver: Espressif (via Volos's port)
- Audio engine: [schreibfaul1/ESP32-audioI2S](https://github.com/schreibfaul1/ESP32-audioI2S)
- Display: [moononournation/Arduino_GFX](https://github.com/moononournation/Arduino_GFX) +
  [LovyanGFX](https://github.com/lovyan03/LovyanGFX)
- Discovery: [Radio-Browser](https://www.radio-browser.info/) community API
