#include "input.h"
#include "config.h"

static bool g_debMid = false;
static bool g_debRight = false;
static bool g_debLeft = false;

void inputBegin() {
    pinMode(PIN_BTN_LEFT,  INPUT_PULLUP);
    pinMode(PIN_BTN_MID,   INPUT_PULLUP);
    pinMode(PIN_BTN_RIGHT, INPUT_PULLUP);
}

InputEvent inputPoll() {
    // Mid: NEXT station. Edge-triggered (rising LOW).
    if (digitalRead(PIN_BTN_MID) == LOW) {
        if (!g_debMid) { g_debMid = true; return INPUT_NEXT; }
    } else {
        g_debMid = false;
    }
    // Right: VOL_UP, wraps in audio module.
    if (digitalRead(PIN_BTN_RIGHT) == LOW) {
        if (!g_debRight) { g_debRight = true; return INPUT_VOL_UP; }
    } else {
        g_debRight = false;
    }
    // Left: SLEEP on a single press -- unless the user is also holding the
    // right button (i.e. they're starting the L+R reboot combo), in which
    // case we suppress SLEEP so inputRebootCombo() can win the race.
    if (digitalRead(PIN_BTN_LEFT) == LOW) {
        if (!g_debLeft) {
            g_debLeft = true;
            if (digitalRead(PIN_BTN_RIGHT) == LOW) return INPUT_NONE;
            return INPUT_SLEEP;
        }
    } else {
        g_debLeft = false;
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
// TODO(touch): pick a driver. If XPT2046: bit-bang or SPI on a shared bus;
// you'll need IRQ pin + chip-select. If GT911: I2C on the existing bus.
// Either way, wire `inputPoll()` to also emit INPUT_TOUCH_TAP events when
// the user lifts the finger so the display module can do hit-testing.
void inputBeginTouch() { /* not yet implemented */ }
bool inputTouchPressed(int *x, int *y) {
    if (x) *x = -1;
    if (y) *y = -1;
    return false;
}
