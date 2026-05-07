# Building from source

## Toolchain

The firmware targets the **pioarduino** fork of `platform-espressif32`
(arduino-esp32 v3.x / ESP-IDF 5.x / GCC 13). The stock `espressif32`
PIO platform pins to arduino-esp32 v2.x (GCC 8) and won't build --
ESP32-audioI2S now uses C++20 headers (`<span>`).

The pioarduino platform is referenced directly in `platformio.ini`:

```ini
platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
```

No global PIO-platform install is needed; PIO will fetch and cache it
on first build.

## Quick build + flash

```
git clone -b claude/waveshare-internet-radio-1syTE \
  https://github.com/GarethDaviesLondon/InternetRadio.git
cd InternetRadio
pio run -t upload
pio device monitor
```

### Default env

`pio run` builds `[env:waveshare-s3-radio]`. To build a different
variant:

```
pio run -e waveshare-s3-radio-rot90 -t upload
```

See [hardware.md](hardware.md) for the list of envs.

## Library dependencies

Pulled automatically via `lib_deps`:

| Library | Why |
| ------- | --- |
| `schreibfaul1/ESP32-audioI2S` | MP3/AAC/OGG/FLAC streaming over I²S to the ES8311 |
| `moononournation/Arduino_GFX` | ST7789 panel driver |
| `lovyan03/LovyanGFX` | Off-screen sprite + the 7-segment + Noto fonts used in clock / bigclock |

You don't need to install anything by hand; first build downloads them.

## Build flags

Inherited from the default env unless overridden:

```ini
build_flags =
    -DBOARD_HAS_PSRAM
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DCORE_DEBUG_LEVEL=0
    -Wno-deprecated-declarations
```

- `ARDUINO_USB_CDC_ON_BOOT=1` routes `Serial` to the native USB-CDC
  port (so PuTTY and `pio device monitor` use the same COM/tty as
  esptool flashes over).
- `CORE_DEBUG_LEVEL` is an Arduino-IDE-only `#define` that
  `ESP32-audioI2S` consults; PIO doesn't set it by default.
- `DISPLAY_ROTATION` is added by the rot90 env and configures the
  panel orientation in `Arduino_ST7789`'s constructor.

## Pre-build hooks

Two SCons hooks run before each build, defined in `scripts/`:

- **`strip_matter.py`** — Windows-specific. The pioarduino framework
  bundles `esp-matter` / `connectedhomeip` headers under a path long
  enough to overflow MSYS2's GCC binary even with
  `LongPathsEnabled=1`. The script strips those entries from
  `CPPPATH` + `-I` flags before compile. No-op on Linux / macOS.
- **`ccache.py`** — wraps `CC` and `CXX` with `ccache` if it's on
  PATH. No-op cleanly when ccache isn't installed.

## ccache

Big wins for incremental builds, especially the heavy LovyanGFX +
Arduino_GFX libraries that recompile on every libdeps churn.

### Install

| OS | One-liner |
| -- | --------- |
| Windows (winget) | `winget install ccache.ccache` |
| Windows (scoop)  | `scoop install ccache` |
| Windows (choco)  | `choco install ccache` |
| macOS            | `brew install ccache` |
| Linux            | `apt install ccache` (or distro equivalent) |

Then **open a new shell** so PATH refreshes, and verify:

```
ccache --version
```

The next `pio run` will print `ccache: enabled via <path>` near the
top.

### Tuning

- `setx CCACHE_DIR "%USERPROFILE%\.ccache"` (Windows) puts the cache
  somewhere stable so it survives `.pio` deletion.
- `setx CCACHE_MAXSIZE 5G` allows ccache to use 5 GiB before evicting.
- `ccache -s` shows hit / miss stats; `ccache -C` clears.

## Common build problems

### "GCC compile-all-the-things"

Arduino_GFX has ~75 panel/databus drivers and compiles every one
into the binary. ccache makes repeat builds nearly instant. If
your libdeps cache keeps getting invalidated between runs:

- Add `.pio` to **Windows Defender** (or your antivirus) exclusion
  list -- real-time scanning rewrites file mtimes and busts SCons.
- Make sure the project isn't inside a OneDrive / Dropbox synced
  folder.
- Confirm you're not accidentally running `pio run -t clean`.

### Long-path errors on Windows (`fatal error: file name too long`)

The `strip_matter.py` pre-script handles this by removing the worst
offenders. If you see it for a different sub-tree, edit the script
and add the path prefix to the exclusion list.

### "`Audio.h` not found" or case-mismatch

The ES8311 driver lives in `radio_audio.{h,cpp}`. The original Volos
sketch had `audio.{h,cpp}` which collides with the library's own
`Audio.h` on case-insensitive filesystems (Windows / macOS-default).
We renamed it to `radio_audio` to fix that. Don't put back an
`audio.h` in `winRadio/`.

### NVS leftovers after a reflash

`pio run -t upload` does NOT erase the NVS partition. So edits to
`stations_defaults.h` won't show up until you run `station reset-all`
in the CLI (or `pio run -t erase` for a full nuke -- which also
wipes saved WiFi credentials).

### `monitor_port = COM5`

The `platformio.ini` pins COM5 because that's the dev machine's USB
enumeration. On other machines `pio device list` shows the right
port; either edit `platformio.ini` locally or override on the command
line: `pio device monitor -p COM7`.

## Arduino IDE

Buildable from the Arduino IDE too if you prefer:

1. Install the **ESP32 Arduino core** (v3.x).
2. Install the libraries listed above via Library Manager.
3. Board: *ESP32S3 Dev Module*, PSRAM: *OPI PSRAM*, Flash size:
   *16 MB*, Partition: *Default 4MB with spiffs*, USB CDC On Boot:
   *Enabled*.
4. Open `winRadio/winRadio.ino` and upload.

The Arduino IDE doesn't run our pre-build scripts, so the rot90 env
isn't reachable -- you'd have to add `-DDISPLAY_ROTATION=1` manually
to the build flags or edit `config.h`.
