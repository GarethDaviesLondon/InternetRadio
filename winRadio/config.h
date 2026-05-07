// Hardware configuration: pin map and codec / sample-rate constants.
// Kept header-only so every module can pick up the same definitions without
// needing a .cpp dependency.

#pragma once

// ---- Audio codec (ES8311) ------------------------------------------------
#define PIN_PA_CTRL   7
#define PIN_I2S_MCLK  8
#define PIN_I2S_BCLK  9
#define PIN_I2S_DOUT  12
#define PIN_I2S_LRC   10

#define PIN_I2C_SDA   42
#define PIN_I2C_SCL   41

// Codec clocking. ES8311 is master-slave: ESP32 generates MCLK, codec locks.
#define AUDIO_SAMPLE_RATE      16000
#define AUDIO_MCLK_MULTIPLE    256
#define AUDIO_MCLK_FREQ_HZ     (AUDIO_SAMPLE_RATE * AUDIO_MCLK_MULTIPLE)
#define AUDIO_CODEC_DEF_VOL    75        // ES8311 digital volume 0..255

// ---- Display (ST7789 240x240 SPI) ----------------------------------------
#define PIN_LCD_DC    45
#define PIN_LCD_CS    21
#define PIN_LCD_SCK   38
#define PIN_LCD_MOSI  39
#define PIN_LCD_RST   40
#define PIN_LCD_BL    46

#define DISPLAY_W     240
#define DISPLAY_H     240

// Panel rotation passed to the Arduino_GFX driver.
//   0 = native orientation (default)
//   1 = 90 deg clockwise
//   2 = 180 deg
//   3 = 270 deg clockwise (= 90 deg counter-clockwise)
// Override per-build via -DDISPLAY_ROTATION=N in platformio.ini.
// Used by enclosure variants where the panel had to be mounted at
// a different angle.
#ifndef DISPLAY_ROTATION
#define DISPLAY_ROTATION 0
#endif

// ---- Input: buttons (active-low) -----------------------------------------
#define PIN_BTN_LEFT  0    // also used as deep-sleep wake
#define PIN_BTN_MID   5
#define PIN_BTN_RIGHT 4

// ---- Power -----------------------------------------------------------------
#define PIN_BAT_EN    2    // hold-latched output enabling battery divider
#define PIN_BAT_ADC   1    // ADC1 channel reading divided battery voltage
#define BAT_DIV_RATIO 3.0f // divider: ADC mV * ratio = battery mV
#define BAT_V_MIN     3.0f
#define BAT_V_MAX     4.2f

// ---- Net / discovery -------------------------------------------------------
#define DEFAULT_HOSTNAME   "WebRadio"                 // mDNS: WebRadio.local
#define PROVISION_AP_SSID  "ON8CIT-WinRadio-Setup"
#define PROVISION_AP_PASS  ""                        // empty = open AP

// ---- Version ---------------------------------------------------------------
#define FIRMWARE_NAME    "ON8CIT WebRadio"
#define FIRMWARE_VERSION "0.3.0"
