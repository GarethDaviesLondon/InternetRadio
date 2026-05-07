# Hardware

## Target board

The firmware is developed against a **Waveshare ESP32-S3** dev board
with onboard 1.3" 240×240 ST7789 SPI LCD and ES8311 audio codec --
the same module as Volos Projects' WaveshareRadioStream.

Variants of the same module ship with extra peripherals (microphones,
6-axis IMU). The firmware probes them at boot and uses what it
finds; missing peripherals just disable their feature.

| Component         | Notes |
| ----------------- | ----- |
| ESP32-S3          | 16 MB flash, 8 MB octal PSRAM, native USB |
| ST7789 LCD        | 240×240, SPI, IPS |
| ES8311 codec      | I²C control + I²S audio (16 kHz, mono) |
| QMI8658 IMU       | I²C; auto-detected at 0x6A or 0x6B |
| Buttons           | Three momentary switches, active-low to ground |
| Battery monitor   | Resistor divider on ADC1; enable via GPIO 2 |
| SD card (4-bit)   | SD_MMC interface; optional |

## Pin map

All pins are defined in `winRadio/config.h`; this is the canonical
list.

### Audio (ES8311 + I²S)

| Function     | GPIO |
| ------------ | ---- |
| Amp enable (PA) | 7  |
| I²S MCLK     | 8    |
| I²S BCLK     | 9    |
| I²S LRC      | 10   |
| I²S DOUT     | 12   |

### I²C (shared by ES8311 + IMU)

| Function | GPIO |
| -------- | ---- |
| SDA      | 42   |
| SCL      | 41   |

The IMU is auto-probed at I²C addresses `0x6A` and `0x6B`. If found,
QMI8658 is initialised for ±2 g accelerometer at 125 Hz; gyro is
left off (we don't need it for the orientation gestures).

### Display (ST7789 SPI)

| Function   | GPIO |
| ---------- | ---- |
| LCD DC     | 45   |
| LCD CS     | 21   |
| LCD SCK    | 38   |
| LCD MOSI   | 39   |
| LCD RST    | 40   |
| Backlight  | 46   |

### Buttons

| Function  | GPIO | Notes |
| --------- | ---- | ----- |
| Left      | 0    | Also wired to ext0 deep-sleep wake |
| Mid       | 5    |       |
| Right     | 4    |       |

All buttons use internal pull-ups; press = LOW.

### Power / battery

| Function          | GPIO |
| ----------------- | ---- |
| Battery enable    | 2    |
| Battery ADC       | 1    |

`PIN_BAT_EN` is held HIGH across deep sleep with `gpio_hold_en` so the
divider remains alive on wake. Voltage is computed as
`adc_mV * BAT_DIV_RATIO / 1000`, where `BAT_DIV_RATIO = 3.0`.

### SD card (SD_MMC, optional)

| Function | GPIO |
| -------- | ---- |
| CLK      | 16   |
| CMD      | 15   |
| DAT0     | 17   |
| DAT1     | 18   |
| DAT2     | 13   |
| DAT3     | 14   |

Used for: optional `/stations.csv` first-boot seed, `/theme.ini`
override, `/log.txt` rotated mirror of the serial log.

## Build variants

| PIO env                       | What changes |
| ----------------------------- | ------------ |
| `waveshare-s3-radio`          | Stock build (`DISPLAY_ROTATION=0`) |
| `waveshare-s3-radio-rot90`    | Panel rotated 90° clockwise (`DISPLAY_ROTATION=1`); same hardware, different enclosure mounting |

To build the rotated variant:

```
pio run -e waveshare-s3-radio-rot90
pio run -e waveshare-s3-radio-rot90 -t upload
```

To add another rotation:

```ini
[env:waveshare-s3-radio-rot180]
extends = env:waveshare-s3-radio
build_flags =
    ${env:waveshare-s3-radio.build_flags}
    -DDISPLAY_ROTATION=2
```

`DISPLAY_ROTATION` values follow Arduino_GFX convention:

| Value | Effect |
| ----- | ------ |
| 0     | native (default) |
| 1     | 90° clockwise |
| 2     | 180° |
| 3     | 90° counter-clockwise |

The panel is square (240×240), so dimensions don't change between
rotations -- only the on-screen origin shifts. All draw code is
unchanged.

## Known board variant differences

Older Waveshare modules may not include the IMU or microphones. The
firmware logs an I²C scan at boot:

```
imu: I2C scan -> 0x18 0x6A
imu: QMI8658C detected at 0x6A (id 0x05)
```

If you see a chip ID at `0x6A` / `0x6B` that doesn't match QMI8658
(0x05), share the hex value and we can add a driver for it. The
rest of the firmware works without an IMU.

## First-flash setup

After flashing the firmware to a fresh module:

1. Connect a battery (or USB-C power) and watch the LCD for the boot
   splash.
2. The radio comes up with the compiled-default station list (see
   [stations\_defaults.h](../winRadio/stations_defaults.h)) and no
   saved WiFi networks.
3. The device starts a SoftAP `ON8CIT-WinRadio-Setup`. Connect a
   phone or laptop to that AP and visit `http://192.168.4.1` (most
   captive-portal-aware OSes auto-open it).
4. Pick the network you want to join from the list, type the
   password, save. The radio reboots and joins the new network.
5. Once associated, audio starts and the device is reachable at the
   STA IP and `http://WebRadio.local`.

The whole flow is also drivable from the USB serial CLI -- handy for
debugging or for headless setup. See [cli.md](cli.md).
