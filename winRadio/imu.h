// 6-axis IMU (accel + gyro). Used for gesture-based UX:
//   - imuMotionEvent()  : one-shot; true once per motion episode
//   - imuFaceDown()     : current orientation (face-down transition)
//   - imuFaceUp()       : current orientation (face-up transition)
//
// The driver auto-detects the sensor by probing known I2C addresses
// and reading chip-ID registers. Currently recognises the QMI8658C
// (Waveshare ESP32-S3 "radio" variants with mic + IMU). On boards
// without a matching IMU the module logs once at boot and becomes a
// no-op -- every query returns false -- so the rest of the firmware
// keeps working.

#pragma once

#include <Arduino.h>

bool imuBegin();          // detect + init. false if no sensor found.
bool imuPresent();        // was a sensor found at boot?
void imuLoop();           // call every ~100-250 ms from the main loop

// Motion wake: edge-triggered. Returns true ONCE per "burst" of
// movement (the unit gets picked up, tapped, shaken, etc.). Reset to
// false on the next quiet tick.
bool imuMotionEvent();

// Orientation: edge-triggered transitions between face-down and
// face-up. Each event returns true exactly once per flip; subsequent
// polls return false until the next flip. face-down = accelerometer
// Z axis points AWAY from gravity (i.e. screen facing the table).
bool imuFaceDownEvent();
bool imuFaceUpEvent();

// Current steady-state orientation, in case a caller wants to poll
// rather than react to edges.
bool imuIsFaceDown();
bool imuIsFaceUp();
