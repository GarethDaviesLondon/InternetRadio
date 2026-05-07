#include "input.h"
#include "config.h"

// All three buttons run the same state machine. The Left machine keeps
// its existing semantics (short=mode-toggle, double=play/pause,
// long=sleep). Mid + Right are now also short/double/long capable to
// support long-press into the new system-info / station-picker modes.
//
// As a result NEXT (mid) and VOL_UP (right) now fire on RELEASE rather
// than on press, after the kDoubleGapMs window has expired (so we know
// it wasn't a double-click). Right has no meaningful double-click, so
// it skips the wait and fires on release immediately.
//
// Wake from deep sleep is handled by ext0 on the Left pin (see
// power.cpp); the wake press is consumed by the wake event and the
// gesture machine starts fresh on the next press.

namespace {

enum BtnState {
    B_IDLE,
    B_FIRST_DOWN,     // pressed; waiting to see if release or long-hold
    B_FIRST_UP_WAIT,  // released; waiting up to kDoubleGapMs for 2nd press
    B_LONG_FIRED,     // long-press fired; wait for release before re-arming
    B_COMBO_ACTIVE,   // suppressed (left+right combo, etc.)
};

struct Btn {
    BtnState  state;
    uint32_t  downAt;
    uint32_t  upAt;
};

Btn s_left  = {B_IDLE, 0, 0};
Btn s_mid   = {B_IDLE, 0, 0};
Btn s_right = {B_IDLE, 0, 0};

constexpr uint32_t kLongPressMs  = 2000;
constexpr uint32_t kDoubleGapMs  =  350;

// Tick one button's state machine. `wantDouble` enables the
// "wait for second click before firing short" behaviour. Returns the
// emitted event for this tick (INPUT_NONE if nothing happened).
//
// `comboPressed` is true when the *other* combo-mate button is also
// down (Left for Right, Right for Left), and steers the button into
// B_COMBO_ACTIVE so the reboot-combo can win the race.
InputEvent tickButton(Btn &b, bool low, uint32_t now,
                      bool comboPressed,
                      InputEvent shortEv,
                      InputEvent longEv,
                      InputEvent doubleEv,
                      bool wantDouble) {
    switch (b.state) {
        case B_IDLE:
            if (low) {
                b.state  = B_FIRST_DOWN;
                b.downAt = now;
            }
            break;

        case B_FIRST_DOWN:
            if (comboPressed) { b.state = B_COMBO_ACTIVE; break; }
            if (!low) {
                b.upAt  = now;
                if (!wantDouble) {
                    // No double-click for this button; fire short on release.
                    b.state = B_IDLE;
                    return shortEv;
                }
                b.state = B_FIRST_UP_WAIT;
                break;
            }
            if (longEv != INPUT_NONE && (now - b.downAt) >= kLongPressMs) {
                b.state = B_LONG_FIRED;
                return longEv;
            }
            break;

        case B_FIRST_UP_WAIT:
            if (low) {
                if ((now - b.upAt) <= kDoubleGapMs) {
                    b.state = B_LONG_FIRED;   // consume until release
                    return doubleEv;
                }
                b.state  = B_FIRST_DOWN;
                b.downAt = now;
                break;
            }
            if ((now - b.upAt) > kDoubleGapMs) {
                b.state = B_IDLE;
                return shortEv;
            }
            break;

        case B_LONG_FIRED:
            if (!low) b.state = B_IDLE;
            break;

        case B_COMBO_ACTIVE:
            if (!low && !comboPressed) b.state = B_IDLE;
            break;
    }
    return INPUT_NONE;
}
} // namespace

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

    // Left first: keeps priority for the reboot-combo + sleep behaviour.
    InputEvent evL = tickButton(s_left,  leftLow,  now,
                                /*combo=*/rightLow,
                                INPUT_MODE_TOGGLE, INPUT_SLEEP, INPUT_PLAY_PAUSE,
                                /*wantDouble=*/true);
    if (evL != INPUT_NONE) return evL;

    InputEvent evM = tickButton(s_mid,   midLow,   now,
                                /*combo=*/false,
                                INPUT_NEXT, INPUT_PICKER_OPEN, INPUT_PICKER_SELECT,
                                /*wantDouble=*/true);
    if (evM != INPUT_NONE) return evM;

    InputEvent evR = tickButton(s_right, rightLow, now,
                                /*combo=*/leftLow,
                                INPUT_VOL_UP, INPUT_SYS_INFO, INPUT_NONE,
                                /*wantDouble=*/false);
    if (evR != INPUT_NONE) return evR;

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
