// User input: physical buttons today, capacitive touch tomorrow.
//
// The module emits semantic events rather than raw GPIO state so the touch
// driver can produce the same events when wired up.

#pragma once

#include <Arduino.h>

enum InputEvent {
    INPUT_NONE = 0,
    INPUT_PREV,        // (touch only, future) -- previous station
    INPUT_NEXT,        // mid button SHORT press: next station / picker advance focus
    INPUT_VOL_UP,      // right button SHORT press
    INPUT_VOL_DOWN,    // (touch only, future)
    INPUT_SLEEP,       // left button LONG press -> deep sleep
    INPUT_MODE_TOGGLE, // left button SHORT press -> cycle display mode
    INPUT_PLAY_PAUSE,  // left button DOUBLE click -> toggle audio
    INPUT_PICKER_OPEN, // mid button LONG press -> open station picker
    INPUT_PICKER_SELECT,// mid button DOUBLE click -> picker confirm
    INPUT_SYS_INFO,    // right button LONG press -> open setup info screen
    INPUT_TOUCH_TAP,   // raw touch event payload (future)
};

void       inputBegin();
InputEvent inputPoll();          // returns one event per call, INPUT_NONE if none

// Raw level checks (bypass the edge detector) -- useful while setup blocks
// the main loop, e.g. polling "is the right button pressed during boot?".
bool       inputLeftHeld();
bool       inputMidHeld();
bool       inputRightHeld();

// True if LEFT + RIGHT have been held together continuously for at least
// `holdMs` milliseconds. Call from loop(); state is tracked internally.
// Intended for an on-device soft-reboot combo (the radio has an internal
// battery, so a USB power-cycle isn't always available).
bool       inputRebootCombo(uint32_t holdMs = 3000);

// Touch screen stub. The Waveshare LCD-1.3 in the Volos build is buttons
// only, but newer Waveshare radios ship the same MCU with an XPT2046 (or
// GT911 capacitive) touch overlay. Bring the driver up here.
void       inputBeginTouch();    // stub: no-op
bool       inputTouchPressed(int *x = nullptr, int *y = nullptr);
