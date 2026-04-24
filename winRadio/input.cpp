#include "input.h"
#include "config.h"

// Left-button gesture state machine
// --------------------------------------------------------------------
//   Short press (release before kLongPressMs) ............. MODE_TOGGLE
//   Double-click (two short presses within kDoubleGapMs) .. PLAY_PAUSE
//   Long press (held >= kLongPressMs) ..................... SLEEP
//   Held while Right is also held ......................... (combo;
//       suppressed here so inputRebootCombo() can win the race.)
//
// Wake from deep sleep is handled by ext0 on the same pin (see
// power.cpp) -- the wake press itself is consumed by the wake event
// and the normal gesture machine starts fresh on the next press.
//
// The mid and right buttons stay simple edge detectors, same as before.

static bool g_debMid   = false;
static bool g_debRight = false;

// Left-button machine.
enum LeftState {
    L_IDLE,
    L_FIRST_DOWN,     // pressed, waiting to see if release or long-hold
    L_FIRST_UP_WAIT,  // released; waiting up to kDoubleGapMs for a 2nd press
    L_COMBO_ACTIVE,   // R came down while L held -- hand over to reboot combo
    L_LONG_FIRED,     // long-press fired; wait for release before re-arming
};
static LeftState  s_left        = L_IDLE;
static uint32_t   s_leftDownAt  = 0;
static uint32_t   s_leftUpAt    = 0;

constexpr uint32_t kLongPressMs  = 2000;   // hold time that counts as "long"
constexpr uint32_t kDoubleGapMs  =  350;   // max gap between short presses

void inputBegin() {
    pinMode(PIN_BTN_LEFT,  INPUT_PULLUP);
    pinMode(PIN_BTN_MID,   INPUT_PULLUP);
    pinMode(PIN_BTN_RIGHT, INPUT_PULLUP);
}

InputEvent inputPoll() {
    bool leftLow  = digitalRead(PIN_BTN_LEFT)  == LOW;
    bool midLow   = digitalRead(PIN_BTN_MID)   == LOW;
    bool rightLow = digitalRead(PIN_BTN_RIGHT) == LOW;
    uint32_t now  = millis();

    // Mid: NEXT station (falling-edge trigger).
    if (midLow) {
        if (!g_debMid) { g_debMid = true; return INPUT_NEXT; }
    } else {
        g_debMid = false;
    }
    // Right: VOL_UP. Suppressed while Left is also held so the combo
    // hold doesn't spam volume events.
    if (rightLow) {
        if (!g_debRight && !leftLow) { g_debRight = true; return INPUT_VOL_UP; }
    } else {
        g_debRight = false;
    }

    // Left-button state machine.
    switch (s_left) {
        case L_IDLE:
            if (leftLow) {
                s_left       = L_FIRST_DOWN;
                s_leftDownAt = now;
            }
            break;

        case L_FIRST_DOWN:
            if (rightLow) { s_left = L_COMBO_ACTIVE; break; }
            if (!leftLow) {
                // Released. If this was already a long hold we wouldn't
                // be here (L_LONG_FIRED intercepts). Must be short; wait
                // for a possible second click.
                s_leftUpAt = now;
                s_left     = L_FIRST_UP_WAIT;
                break;
            }
            if (now - s_leftDownAt >= kLongPressMs) {
                s_left = L_LONG_FIRED;
                return INPUT_SLEEP;      // fires while button still held
            }
            break;

        case L_FIRST_UP_WAIT:
            if (leftLow) {
                if (now - s_leftUpAt <= kDoubleGapMs) {
                    // Second press arrived within the gap -- double-click.
                    s_left = L_LONG_FIRED;   // consume until release
                    return INPUT_PLAY_PAUSE;
                }
                // Too late to be a double; treat as a fresh press.
                s_left       = L_FIRST_DOWN;
                s_leftDownAt = now;
                break;
            }
            if (now - s_leftUpAt > kDoubleGapMs) {
                // No second press came; it was a single short press.
                s_left = L_IDLE;
                return INPUT_MODE_TOGGLE;
            }
            break;

        case L_LONG_FIRED:
            if (!leftLow && !rightLow) s_left = L_IDLE;
            break;

        case L_COMBO_ACTIVE:
            // Both L and R were held together; the reboot combo owns
            // this interval. Return to idle once both are released.
            if (!leftLow && !rightLow) s_left = L_IDLE;
            break;
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
#ifndef USE_TOUCH_XPT2046
void inputBeginTouch() {}
bool inputTouchPressed(int *x, int *y) {
    if (x) *x = -1;
    if (y) *y = -1;
    return false;
}
#endif
