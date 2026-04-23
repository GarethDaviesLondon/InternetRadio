#include "log.h"
#include "storage.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <FS.h>
#include <stdarg.h>

namespace {
bool      s_verbose    = false;
bool      s_sdEnabled  = false;
String    s_sdPath     = "/log.txt";
uint32_t  s_sdMaxBytes = 0;
uint8_t   s_sdKeep     = 0;

const char *levelTag(LogLevel l) {
    switch (l) {
        case LOG_BOOT:  return "[BOOT] ";
        case LOG_INFO:  return "[INFO] ";
        case LOG_DEBUG: return "[DEBUG]";
        case LOG_ERROR: return "[ERROR]";
        default:        return "[?]    ";
    }
}

void rotateIfNeeded(size_t incomingBytes) {
    if (!s_sdEnabled) return;
    if (s_sdMaxBytes == 0) return;
    uint32_t sz = 0;
    File f = SD_MMC.open(s_sdPath.c_str(), FILE_READ);
    if (f) { sz = f.size(); f.close(); }
    if (sz + incomingBytes < s_sdMaxBytes) return;

    // Shift log.N-1 -> log.N. Drop the oldest.
    for (int i = s_sdKeep - 1; i >= 0; i--) {
        String cur  = s_sdPath + "." + String(i);
        String next = s_sdPath + "." + String(i + 1);
        if (SD_MMC.exists(cur.c_str())) {
            if (i + 1 >= s_sdKeep) SD_MMC.remove(cur.c_str());
            else                   SD_MMC.rename(cur.c_str(), next.c_str());
        }
    }
    String first = s_sdPath + ".0";
    if (SD_MMC.exists(s_sdPath.c_str())) SD_MMC.rename(s_sdPath.c_str(), first.c_str());
}

void writeToSd(const char *line, size_t len) {
    if (!s_sdEnabled) return;
    rotateIfNeeded(len);
    File f = SD_MMC.open(s_sdPath.c_str(), FILE_APPEND);
    if (!f) return;
    f.write((const uint8_t *)line, len);
    f.close();
}
} // namespace

void logBegin()                { /* Serial is already set up by main setup(). */ }
bool logVerbose()              { return s_verbose; }
void logSetVerbose(bool on)    { s_verbose = on; }

bool logSdBegin(const char *path, uint32_t maxBytes, uint8_t keep) {
    if (!storageSdMounted()) return false;
    s_sdPath     = path;
    s_sdMaxBytes = maxBytes;
    s_sdKeep     = keep;
    // Probe-write: make sure the file is appendable.
    File f = SD_MMC.open(s_sdPath.c_str(), FILE_APPEND);
    if (!f) return false;
    f.close();
    s_sdEnabled = true;
    return true;
}

void logSdEnd()     { s_sdEnabled = false; }
bool logSdEnabled() { return s_sdEnabled; }

void logLine(LogLevel lvl, const char *fmt, ...) {
    if (lvl == LOG_DEBUG && !s_verbose) return;

    char stack[256];
    va_list ap;
    va_start(ap, fmt);
    int msgLen = vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    if (msgLen < 0) return;
    if ((size_t)msgLen >= sizeof(stack)) msgLen = sizeof(stack) - 1;

    // Assemble "[LEVEL] <msg>\r\n".
    char line[320];
    int n = snprintf(line, sizeof(line), "%s %s\r\n", levelTag(lvl), stack);
    if (n < 0) return;
    if ((size_t)n >= sizeof(line)) n = sizeof(line) - 1;

    Serial.write((const uint8_t *)line, n);
    writeToSd(line, n);
}
