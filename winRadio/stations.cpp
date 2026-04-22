#include "stations.h"
#include "storage.h"

#include <Arduino.h>

namespace {
struct Preset { const char *name; const char *url; };

// Compiled-in presets. Kept as the fallback when SD is absent or
// /stations.csv is missing / empty / unparseable.
const Preset kDefaults[] = {
    { "SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3"        },
    { "Disco Diamond",       "https://discodiamond.radioca.st/autodj"           },
    { "Radio King 175279",   "https://listen.radioking.com/radio/175279/stream/216784" },
    { "Radio Caroline",      "http://sc6.radiocaroline.net:8040/stream"         },
    { "Raute Musik Club",    "https://club-high.rautemusik.fm/;"                },
    { "WGMC Jazz",           "http://greece-media.monroe.edu/wgmc.mp3"          },
    { "Radio Banovina",      "https://audio.radio-banovina.hr:9998/;"           },
    { "Radio Paradise",      "http://stream.radioparadise.com/mp3-128"          },
};
constexpr int kDefaultCount = sizeof(kDefaults) / sizeof(kDefaults[0]);

// In-memory list (populated from SD when available, otherwise points at
// the compiled defaults). We cap at 32 entries to keep RAM predictable.
constexpr int kMaxStations = 32;
struct Entry { String name; String url; };
Entry s_list[kMaxStations];
int   s_count   = 0;
bool  s_fromSd  = false;
} // namespace

int stationsCount() {
    return s_fromSd ? s_count : kDefaultCount;
}

const char *stationsUrl(int idx) {
    if (s_fromSd) {
        if (idx < 0 || idx >= s_count) return "";
        return s_list[idx].url.c_str();
    }
    if (idx < 0 || idx >= kDefaultCount) return "";
    return kDefaults[idx].url;
}

const char *stationsName(int idx) {
    if (s_fromSd) {
        if (idx < 0 || idx >= s_count) return "";
        return s_list[idx].name.c_str();
    }
    if (idx < 0 || idx >= kDefaultCount) return "";
    return kDefaults[idx].name;
}

void stationsUseDefaults() {
    s_count  = 0;
    s_fromSd = false;
}

int stationsLoadFromSd(const char *path) {
    s_count  = 0;
    s_fromSd = false;

    if (!storageSdExists(path)) return 0;
    String text;
    if (!storageSdReadText(path, text, 16384)) return 0;

    int start = 0;
    while (start < (int)text.length() && s_count < kMaxStations) {
        int nl = text.indexOf('\n', start);
        String line = (nl < 0) ? text.substring(start) : text.substring(start, nl);
        start = (nl < 0) ? text.length() : nl + 1;
        // Strip trailing CR (Windows line endings).
        if (line.endsWith("\r")) line.remove(line.length() - 1);
        line.trim();
        if (line.length() == 0 || line.startsWith("#") || line.startsWith(";")) continue;

        int comma = line.indexOf(',');
        if (comma <= 0) continue;
        String name = line.substring(0, comma); name.trim();
        String url  = line.substring(comma + 1); url.trim();
        if (url.length() == 0) continue;
        s_list[s_count].name = name.length() ? name : url;
        s_list[s_count].url  = url;
        s_count++;
    }

    if (s_count == 0) return 0;
    s_fromSd = true;
    Serial.printf("stations: loaded %d from %s\r\n", s_count, path);
    return s_count;
}
