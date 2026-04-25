#include "imu.h"
#include "config.h"

#include <Arduino.h>
#include <Wire.h>

namespace {

// ---- Driver registry ----------------------------------------------------
// Only QMI8658C today. The driver is written so new chips can slot in by
// adding another entry to kProbes[] + an init/read implementation.

enum ChipKind { CHIP_NONE = 0, CHIP_QMI8658 };
struct Probe {
    const char *name;
    uint8_t     addr;
    uint8_t     idReg;
    uint8_t     idValue;
    ChipKind    kind;
};
const Probe kProbes[] = {
    // QMI8658C: WHO_AM_I at reg 0x00 returns 0x05. Two addresses are
    // possible depending on the board's SA0 strap.
    { "QMI8658C", 0x6B, 0x00, 0x05, CHIP_QMI8658 },
    { "QMI8658C", 0x6A, 0x00, 0x05, CHIP_QMI8658 },
};

// ---- State --------------------------------------------------------------

bool     s_present = false;
ChipKind s_chip    = CHIP_NONE;
uint8_t  s_addr    = 0x00;

// Smoothed accel in "g" units. Exponential moving average (alpha ~0.2).
float    s_emaX = 0, s_emaY = 0, s_emaZ = 0;
bool     s_emaPrimed = false;

// Orientation state, held once stable for 300 ms.
enum OrientationState { ORI_UNKNOWN, ORI_FACE_UP, ORI_FACE_DOWN, ORI_SIDE };
OrientationState s_ori          = ORI_UNKNOWN;
OrientationState s_pendingOri   = ORI_UNKNOWN;
uint32_t         s_pendingSince = 0;

// Edge-triggered event flags. Set when the relevant transition is
// observed; cleared when the caller reads them.
bool     s_evMotion   = false;
bool     s_evFaceUp   = false;
bool     s_evFaceDown = false;

// Motion detection: previous accel magnitude for frame-to-frame delta.
float    s_prevMag    = 1.0f;
uint32_t s_motionUntil = 0;    // millis() until which motion is "hot"

uint32_t s_lastPoll   = 0;

// ---- Low-level I2C helpers ---------------------------------------------

bool readReg(uint8_t reg, uint8_t *buf, size_t len) {
    Wire.beginTransmission(s_addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    size_t got = Wire.requestFrom(s_addr, (uint8_t)len);
    if (got != len) return false;
    for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
    return true;
}
bool writeReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(s_addr);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission(true) == 0;
}

// ---- QMI8658 driver ----------------------------------------------------
// Registers we touch:
//   0x02 CTRL1   : 0x60 = auto-increment + SPI big-endian (don't care, we
//                  use I2C; 0x60 ensures burst reads work on both endian).
//   0x03 CTRL2   : accel ODR + FS. 0x04 = aODR=125 Hz, aFS=+/-2 g.
//   0x04 CTRL3   : gyro ODR + FS. We leave gyro off -> 0x00.
//   0x06 CTRL5   : LPF. 0x00 = default filters.
//   0x08 CTRL7   : enable bits. 0x01 = accel on, gyro off.
//   0x35..0x3A   : AX L/H, AY L/H, AZ L/H (int16 little-endian, g = raw/16384
//                  at +/-2 g full scale).

bool qmiInit() {
    if (!writeReg(0x02, 0x60)) return false;
    if (!writeReg(0x03, 0x04)) return false;  // accel 125 Hz, +/-2 g
    if (!writeReg(0x04, 0x00)) return false;  // gyro off
    if (!writeReg(0x06, 0x00)) return false;
    if (!writeReg(0x08, 0x01)) return false;  // accel enable
    delay(10);
    return true;
}

bool qmiReadAccelG(float &gx, float &gy, float &gz) {
    uint8_t raw[6];
    if (!readReg(0x35, raw, 6)) return false;
    int16_t ax = (int16_t)((raw[1] << 8) | raw[0]);
    int16_t ay = (int16_t)((raw[3] << 8) | raw[2]);
    int16_t az = (int16_t)((raw[5] << 8) | raw[4]);
    gx = ax / 16384.0f;
    gy = ay / 16384.0f;
    gz = az / 16384.0f;
    return true;
}

// ---- Generic read ------------------------------------------------------

bool readAccelG(float &gx, float &gy, float &gz) {
    switch (s_chip) {
        case CHIP_QMI8658: return qmiReadAccelG(gx, gy, gz);
        default:           return false;
    }
}

// ---- Probe -------------------------------------------------------------

void scanBusAndLog() {
    Serial.print(F("imu: I2C scan ->"));
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission(true) == 0) {
            Serial.printf(" 0x%02X", a);
            found++;
        }
    }
    if (found == 0) Serial.print(F(" (no devices)"));
    Serial.println();
}
} // namespace

bool imuBegin() {
    s_present = false;
    s_chip    = CHIP_NONE;

    // Wire is already initialised in setup() via Wire.begin(SDA, SCL).
    scanBusAndLog();

    for (const auto &p : kProbes) {
        s_addr = p.addr;
        uint8_t id = 0;
        if (!readReg(p.idReg, &id, 1)) continue;
        if (id != p.idValue) continue;
        Serial.printf("imu: %s detected at 0x%02X (id 0x%02X)\r\n",
                      p.name, p.addr, id);
        s_chip = p.kind;
        bool ok = false;
        switch (p.kind) {
            case CHIP_QMI8658: ok = qmiInit(); break;
            default: break;
        }
        if (!ok) {
            Serial.printf("imu: %s init FAILED, continuing without IMU\r\n", p.name);
            s_chip = CHIP_NONE;
            continue;
        }
        s_present = true;
        return true;
    }

    Serial.println(F("imu: no supported sensor found -- IMU features disabled"));
    return false;
}

bool imuPresent() { return s_present; }

void imuLoop() {
    if (!s_present) return;
    uint32_t now = millis();
    if (now - s_lastPoll < 100) return;   // ~10 Hz is plenty for orientation
    s_lastPoll = now;

    float gx, gy, gz;
    if (!readAccelG(gx, gy, gz)) return;

    // Prime the EMA on the first good sample.
    if (!s_emaPrimed) {
        s_emaX = gx; s_emaY = gy; s_emaZ = gz;
        s_emaPrimed = true;
        s_prevMag = sqrtf(gx*gx + gy*gy + gz*gz);
        return;
    }

    // Motion detection: frame-to-frame delta in the accel magnitude.
    // A still unit reads ~1.00 g; any picked-up / tapped / shaken event
    // spikes above 1.15 g or drops below 0.85 g transiently.
    float mag = sqrtf(gx*gx + gy*gy + gz*gz);
    float jerk = fabsf(mag - s_prevMag);
    s_prevMag = mag;
    if (jerk > 0.12f) {
        s_evMotion    = true;
        s_motionUntil = now + 600;
    }

    // Update smoothed gravity vector (for orientation only).
    const float alpha = 0.2f;
    s_emaX += alpha * (gx - s_emaX);
    s_emaY += alpha * (gy - s_emaY);
    s_emaZ += alpha * (gz - s_emaZ);

    // Two-state classifier with hysteresis. We only care about "lying
    // flat face-down on a surface" vs "anything else". Face-down is
    // entered when the gravity vector dominates +Z; we leave face-down
    // when the unit tilts more than ~45 deg off horizontal (any edge,
    // any direction). The face-up event is emitted on the leave
    // transition so the existing main-loop "resume if WE paused"
    // wiring keeps working.
    //
    // Polarity note: on the Waveshare board the accelerometer +Z axis
    // points OUT THE BACK of the case (not the screen), so screen-down
    // reads gravity POSITIVE on Z.
    //
    // Thresholds:
    //   enter face-down: Az > 0.85 g sustained 500 ms (deep, stable)
    //   leave face-down: Az < 0.70 g sustained 300 ms (~45 deg tilt)
    constexpr float kEnterFaceDownG = 0.85f;
    constexpr float kLeaveFaceDownG = 0.70f;
    constexpr uint32_t kEnterDebounceMs = 500;
    constexpr uint32_t kLeaveDebounceMs = 300;

    OrientationState cur;
    if (s_ori == ORI_FACE_DOWN) {
        // Already face-down; only leave when we tilt past the
        // shallower threshold so we don't oscillate.
        cur = (s_emaZ < kLeaveFaceDownG) ? ORI_SIDE : ORI_FACE_DOWN;
    } else {
        cur = (s_emaZ > kEnterFaceDownG) ? ORI_FACE_DOWN : ORI_SIDE;
    }

    if (cur != s_pendingOri) {
        s_pendingOri   = cur;
        s_pendingSince = now;
    }
    uint32_t debounce = (cur == ORI_FACE_DOWN) ? kEnterDebounceMs : kLeaveDebounceMs;
    if (cur != s_ori && (now - s_pendingSince) > debounce) {
        OrientationState was = s_ori;
        s_ori = cur;
        if (cur == ORI_FACE_DOWN && was != ORI_FACE_DOWN) s_evFaceDown = true;
        if (cur != ORI_FACE_DOWN && was == ORI_FACE_DOWN) s_evFaceUp   = true;
    }
}

bool imuMotionEvent() {
    if (!s_evMotion) return false;
    // Latch the event until the motion "hot" window expires, so several
    // quick checks within a burst don't all fire.
    if (millis() > s_motionUntil) s_evMotion = false;
    bool r = s_evMotion;
    s_evMotion = false;
    return r;
}
bool imuFaceDownEvent() { bool r = s_evFaceDown; s_evFaceDown = false; return r; }
bool imuFaceUpEvent()   { bool r = s_evFaceUp;   s_evFaceUp   = false; return r; }

bool imuIsFaceDown() { return s_ori == ORI_FACE_DOWN; }
bool imuIsFaceUp()   { return s_ori == ORI_FACE_UP; }
