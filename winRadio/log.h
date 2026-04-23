// Leveled logging facade.
//
// Prepend every line with a [LEVEL] tag so a serial dump can be filtered
// downstream. Verbose mode (set via CLI `log verbose on|off`) gates the
// DEBUG level; BOOT / INFO / ERROR always print.
//
// SD rotation: when enabled via logSdBegin(path, maxBytes, keep), every
// log line is also appended to a file on SD. When the active file hits
// maxBytes it rolls -- log.0 -> log.1, log.1 -> log.2, ... (kept files
// age off at `keep`). Disabled by default; real writes land once SD is
// mounted and logSdBegin() is called from setup().

#pragma once

#include <Arduino.h>

enum LogLevel {
    LOG_BOOT  = 0,   // one-shot "we reached this state" during setup
    LOG_INFO  = 1,   // steady-state, mildly interesting
    LOG_DEBUG = 2,   // verbose / diagnostics (gated by verbose flag)
    LOG_ERROR = 3,   // failure path
};

// ---- Core API ------------------------------------------------------------
void logBegin();                              // installs Serial as the default sink
void logLine(LogLevel lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// ---- Verbose toggle (controls DEBUG emission) ----------------------------
bool logVerbose();
void logSetVerbose(bool on);

// ---- SD rotation hooks (stubs today, wired to storageSdWriteText later) --
// maxBytes <= 0 disables rotation. keep = how many rotated files to keep.
// Returns true if the sink was armed (SD mounted + path writable).
bool logSdBegin(const char *path = "/log.txt",
                uint32_t maxBytes = 64 * 1024,
                uint8_t keep = 3);
void logSdEnd();
bool logSdEnabled();

// ---- Convenience wrappers ------------------------------------------------
#define LOGB(fmt, ...) logLine(LOG_BOOT,  fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...) logLine(LOG_INFO,  fmt, ##__VA_ARGS__)
#define LOGD(fmt, ...) logLine(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) logLine(LOG_ERROR, fmt, ##__VA_ARGS__)
