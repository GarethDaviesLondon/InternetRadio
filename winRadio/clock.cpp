#include "clock.h"
#include "storage.h"

#include <Arduino.h>
#include <time.h>
#include <sys/time.h>

// Default TZ: Europe/London (GMT in winter, BST in summer, last Sun in
// March / October per EU rules). The user is UK-based; override via the
// web /api/timezone or CLI `time set <posix>`.
static const char *kDefaultTz = "GMT0BST,M3.5.0/1,M10.5.0";
static String      s_tz       = kDefaultTz;

void clockBegin() {
    s_tz = storageGetString("clock", "tz", kDefaultTz);
    if (s_tz.length() == 0) s_tz = kDefaultTz;
    // Multiple NTP servers so a transient DNS / ACL fail on one doesn't
    // block the whole sync.
    configTzTime(s_tz.c_str(),
                 "pool.ntp.org",
                 "time.nist.gov",
                 "time.google.com");
    Serial.printf("clock: tz=\"%s\" SNTP started\r\n", s_tz.c_str());
}

bool clockIsSynced() {
    time_t now = time(nullptr);
    // Before SNTP has replied, time_t is ~0 (1970). Anything past
    // 2020-01-01 (1577836800) means we've received a real time.
    return now > 1577836800;
}

bool clockLocalTime(struct tm *out) {
    if (!clockIsSynced()) return false;
    time_t now = time(nullptr);
    localtime_r(&now, out);
    return true;
}

void clockFormatDate(char *buf, size_t n) {
    if (!buf || n == 0) return;
    struct tm t;
    if (!clockLocalTime(&t)) { buf[0] = 0; return; }
    // "Sat 25-Apr-2026"
    strftime(buf, n, "%a %d-%b-%Y", &t);
}

void clockFormatTime(char *buf, size_t n) {
    if (!buf || n == 0) return;
    struct tm t;
    if (!clockLocalTime(&t)) { buf[0] = 0; return; }
    strftime(buf, n, "%H:%M:%S", &t);
}

const char *clockTimezone() { return s_tz.c_str(); }

bool clockSetTimezone(const char *tz) {
    if (!tz || !*tz) return false;
    s_tz = tz;
    storagePutString("clock", "tz", s_tz);
    // Re-applies both the TZ setting and kicks SNTP to recompute the
    // offset. The wall-time reading is updated immediately.
    configTzTime(s_tz.c_str(),
                 "pool.ntp.org",
                 "time.nist.gov",
                 "time.google.com");
    setenv("TZ", s_tz.c_str(), 1);
    tzset();
    Serial.printf("clock: tz set to \"%s\"\r\n", s_tz.c_str());
    return true;
}
