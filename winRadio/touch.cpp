// XPT2046 resistive-touch scaffold.
//
// DISABLED BY DEFAULT. Define USE_TOUCH_XPT2046 (as a -D build flag in
// platformio.ini) to activate.
//
// Why scaffold rather than "done": the Waveshare ESP32-S3-LCD-1.3 in the
// Volos video has no touch overlay, so every function below compiles to
// a no-op unless the user opts in. If a future board revision or a
// different Waveshare SKU exposes XPT2046 on the existing SPI bus, flip
// the flag, set TOUCH_CS / TOUCH_IRQ to match the wiring, and the module
// will start emitting INPUT_TOUCH_TAP events.
//
// Pin defaults assume XPT2046 shares the LCD SPI bus. If your board uses
// a separate bus (or GT911 over I2C), override in config.h.

#include "input.h"
#include "config.h"
#include <Arduino.h>

#ifdef USE_TOUCH_XPT2046

#include <SPI.h>

#ifndef TOUCH_CS
  #define TOUCH_CS   33   // adjust to your wiring
#endif
#ifndef TOUCH_IRQ
  #define TOUCH_IRQ  36   // -1 if not connected (then we'll poll-read the Z)
#endif

namespace {
constexpr uint8_t CMD_READ_X = 0xD0;  // 12-bit, differential X
constexpr uint8_t CMD_READ_Y = 0x90;  // 12-bit, differential Y
constexpr uint8_t CMD_READ_Z1 = 0xB0;
constexpr uint8_t CMD_READ_Z2 = 0xC0;

bool    s_ready   = false;
int     s_lastX   = -1;
int     s_lastY   = -1;
bool    s_pressed = false;
bool    s_tapEdge = false;   // latched when finger lifts after a press

uint16_t xptRead(uint8_t cmd) {
    digitalWrite(TOUCH_CS, LOW);
    SPI.transfer(cmd);
    uint16_t hi = SPI.transfer(0);
    uint16_t lo = SPI.transfer(0);
    digitalWrite(TOUCH_CS, HIGH);
    // XPT2046 12-bit value is MSB-first across two bytes, top bit always zero.
    return ((hi << 8) | lo) >> 3;
}

int mapCoord(uint16_t raw, int inMin, int inMax, int outMax) {
    if (raw < inMin) raw = inMin;
    if (raw > inMax) raw = inMax;
    long v = (long)(raw - inMin) * outMax / (inMax - inMin);
    if (v < 0) v = 0;
    if (v > outMax) v = outMax;
    return (int)v;
}
} // namespace

void inputBeginTouch() {
    pinMode(TOUCH_CS, OUTPUT);
    digitalWrite(TOUCH_CS, HIGH);
    if (TOUCH_IRQ >= 0) pinMode(TOUCH_IRQ, INPUT_PULLUP);
    // We assume SPI has been init'd by the display module on pins 38/39.
    // XPT2046 tolerates sharing the bus as long as we toggle CS properly.
    s_ready = true;
    Serial.println("touch: XPT2046 scaffold armed");
}

bool inputTouchPressed(int *x, int *y) {
    if (!s_ready) { if (x) *x = -1; if (y) *y = -1; return false; }
    // Cheap presence check: if IRQ is wired and released, nothing to do.
    if (TOUCH_IRQ >= 0 && digitalRead(TOUCH_IRQ) == HIGH) {
        if (s_pressed) { s_tapEdge = true; s_pressed = false; }
        if (x) *x = s_lastX; if (y) *y = s_lastY;
        return false;
    }
    // Read pressure.
    uint16_t z1 = xptRead(CMD_READ_Z1);
    uint16_t z2 = xptRead(CMD_READ_Z2);
    int z = (int)z1 + 4095 - (int)z2;
    if (z < 400) {
        if (s_pressed) { s_tapEdge = true; s_pressed = false; }
        if (x) *x = s_lastX; if (y) *y = s_lastY;
        return false;
    }
    uint16_t rx = xptRead(CMD_READ_X);
    uint16_t ry = xptRead(CMD_READ_Y);
    // Default calibration for a generic XPT2046; run a calibration pass
    // later and drop these into config.h.
    s_lastX = mapCoord(rx, 300, 3800, 239);
    s_lastY = mapCoord(ry, 300, 3800, 239);
    s_pressed = true;
    if (x) *x = s_lastX;
    if (y) *y = s_lastY;
    return true;
}

bool inputTouchTapConsume(int *x, int *y) {
    if (!s_tapEdge) return false;
    s_tapEdge = false;
    if (x) *x = s_lastX;
    if (y) *y = s_lastY;
    return true;
}

#else  // USE_TOUCH_XPT2046 not defined -- scaffold compiles to nothing.

// The stubs in input.cpp already provide no-op inputBeginTouch /
// inputTouchPressed, so we deliberately add no symbols here to avoid
// duplicate-definition errors at link time.

#endif
