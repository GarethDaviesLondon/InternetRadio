// User input: physical buttons today, capacitive touch tomorrow.
//
// The module emits semantic events rather than raw GPIO state so the touch
// driver can produce the same events when wired up.

#pragma once

#include <Arduino.h>

enum InputEvent {
    INPUT_NONE = 0,
    INPUT_PREV,        // mid button (or left tap on touch) -> previous station
    INPUT_NEXT,        // also mid button in current sketch -> next station
    INPUT_VOL_UP,      // right button
    INPUT_VOL_DOWN,    // (touch only, future)
    INPUT_SLEEP,       // left button held -> deep sleep
    INPUT_TOUCH_TAP,   // raw touch event payload (future)
};

void       inputBegin();
InputEvent inputPoll();          // returns one event per call, INPUT_NONE if none

// Touch screen stub. The Waveshare LCD-1.3 in the Volos build is buttons
// only, but newer Waveshare radios ship the same MCU with an XPT2046 (or
// GT911 capacitive) touch overlay. Bring the driver up here.
void       inputBeginTouch();    // stub: no-op
bool       inputTouchPressed(int *x = nullptr, int *y = nullptr);
