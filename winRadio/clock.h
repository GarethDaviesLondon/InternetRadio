// Network-synchronised wall clock. Uses arduino-esp32's configTzTime +
// the SDK's built-in SNTP client. TZ is a POSIX TZ string (the same
// format libc expects for the TZ env var) persisted in NVS so the
// radio comes back at the correct local time after a reboot.

#pragma once

#include <Arduino.h>
#include <time.h>

// One-shot config. Called from setup() once STA is connected. Safe to
// call again if the user changes the timezone via the web UI.
void        clockBegin();

// True once SNTP has locked and time_t is plausibly-2020-or-later.
bool        clockIsSynced();

// Copy the local-time struct tm into `out`. Returns false if not synced.
bool        clockLocalTime(struct tm *out);

// Preformatted "Sat 25-Apr-2026" and "14:32:05" into the caller's buffer.
// Empty string on not-synced.
void        clockFormatDate(char *buf, size_t n);
void        clockFormatTime(char *buf, size_t n);

// Active POSIX TZ string (e.g. "GMT0BST,M3.5.0/1,M10.5.0").
const char *clockTimezone();

// Set timezone, persist to NVS, re-run configTzTime so the SNTP offset
// updates. Returns false if `tz` is empty.
bool        clockSetTimezone(const char *tz);
