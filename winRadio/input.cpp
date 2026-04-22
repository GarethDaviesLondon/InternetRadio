#include "input.h"
#include "config.h"

static bool     g_debMid = false;
static bool     g_debRight = false;
// Left-button state machine: we want SLEEP to fire on the *release* of a
// clean Left-only short press. Firing on press means a user can't start
// the L+R reboot combo -- the first L-down edge would already have
// suspended the device.
static bool     g_leftDown = false;
static bool     g_leftSawCombo = false;   // right was ever held while left was down

void inputBegin() {
    pinMode(PIN_BTN_LEFT,  INPUT_PULLUP);
    pinMode(PIN_BTN_MID,   INPUT_PULLUP);
    pinMode(PIN_BTN_RIGHT, INPUT_PULLUP);
}

InputEvent inputPoll() {
    bool leftLow  = digitalRead(PIN_BTN_LEFT)  == LOW;
    bool midLow   = digitalRead(PIN_BTN_MID)   == LOW;
    bool rightLow = digitalRead(PIN_BTN_RIGHT) == LOW;

    // Mid: NEXT station. Edge-triggered (falling to LOW).
    if (midLow) {
        if (!g_debMid) { g_debMid = true; return INPUT_NEXT; }
    } else {
        g_debMid = false;
    }
    // Right: VOL_UP. Suppressed while the left button is also down so a
    // held R during the combo window doesn't spam volume changes.
    if (rightLow) {
        if (!g_debRight && !leftLow) { g_debRight = true; return INPUT_VOL_UP; }
    } else {
        g_debRight = false;
    }
    // Left: SLEEP on a clean press-and-release, with nothing else pressed
    // during the hold. If R was pressed at any point while L was down we
    // treat it as (the start of) the reboot combo and don't sleep.
    if (leftLow && !g_leftDown) {
        g_leftDown = true;
        g_leftSawCombo = rightLow;
    }
    if (leftLow && rightLow) g_leftSawCombo = true;
    if (!leftLow && g_leftDown) {
        g_leftDown = false;
        bool wasCombo = g_leftSawCombo;
        g_leftSawCombo = false;
        if (!wasCombo) return INPUT_SLEEP;
    }
    return INPUT_NONE;
}

bool inputLeftHeld()  { return digitalRead(PIN_BTN_LEFT)  == LOW; }
bool inputMidHeld()   { return digitalRead(PIN_BTN_MID)   == LOW; }
bool inputRightHeld() { return digitalRead(PIN_BTN_RIGHT) == LOW; }

bool inputRebootCombo(uint32_t holdMs) {
    static uint32_t comboStart = 0;
    bool both = inputLeftHeld() && inputRightHeld();
    if (!both) { comboStart = 0; return false; }
    if (comboStart == 0) comboStart = millis();
    return (millis() - comboStart) >= holdMs;
}

// ---- Touch (stub) --------------------------------------------------------
// Real implementation lives in touch.cpp behind USE_TOUCH_XPT2046. The
// Waveshare ESP32-S3-LCD-1.3 in the Volos video has no touch overlay, so
// the stub here does nothing and keeps INPUT_TOUCH_TAP unemitted.
void inputBeginTouch() {}
bool inputTouchPressed(int *x, int *y) {
    if (x) *x = -1;
    if (y) *y = -1;
    return false;
}
