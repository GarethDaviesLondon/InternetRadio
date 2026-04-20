#include "stations.h"

namespace {
struct Preset { const char *name; const char *url; };

// Mix of plain-HTTP (reliable for the ESP32-audioI2S library) and HTTPS
// streams. Keep one HTTP entry first so boot always has a working option.
const Preset kPresets[] = {
    { "SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3"        },
    { "Disco Diamond",       "https://discodiamond.radioca.st/autodj"           },
    { "Radio King 175279",   "https://listen.radioking.com/radio/175279/stream/216784" },
    { "Radio Caroline",      "http://sc6.radiocaroline.net:8040/stream"         },
    { "Raute Musik Club",    "https://club-high.rautemusik.fm/;"                },
    { "WGMC Jazz",           "http://greece-media.monroe.edu/wgmc.mp3"          },
    { "Radio Banovina",      "https://audio.radio-banovina.hr:9998/;"           },
    { "Radio Paradise",      "http://stream.radioparadise.com/mp3-128"          },
};
constexpr int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);
} // namespace

int stationsCount() { return kPresetCount; }

const char *stationsUrl(int idx) {
    if (idx < 0 || idx >= kPresetCount) return "";
    return kPresets[idx].url;
}

const char *stationsName(int idx) {
    if (idx < 0 || idx >= kPresetCount) return "";
    return kPresets[idx].name;
}
