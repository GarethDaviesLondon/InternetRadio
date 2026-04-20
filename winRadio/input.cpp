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
    // Left: SLEEP. Single press is enough -- power module handles ack.
    if (digitalRead(PIN_BTN_LEFT) == LOW) {
        if (!g_debLeft) { g_debLeft = true; return INPUT_SLEEP; }
    } else {
        g_debLeft = false;
    }
    return INPUT_NONE;
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
