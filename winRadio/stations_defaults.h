// Compile-time default station list.
//
// Edit THIS file to change the stations the radio ships with. The
// list is used as the seed for first-boot population, when neither
// SD /stations.csv nor a previously-saved NVS list exists.
//
// Format: each entry is { "Friendly name", "Stream URL" }. Streams
// must be MP3, AAC, OGG, FLAC, or WAV over HTTP / HTTPS. The audio
// engine (schreibfaul1's ESP32-audioI2S) handles all five.
//
// Tip: keep the list short (~8) so a brand-new flash boots into a
// usable state quickly. Users will then add their own via Discover
// or the web UI; those additions persist in NVS regardless of what
// this file contains.

#pragma once

namespace stationsDefaults {

struct Preset { const char *name; const char *url; };

inline constexpr Preset kPresets[] = {
    { "SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3"        },
    { "Disco Diamond",       "https://discodiamond.radioca.st/autodj"           },
    { "Radio King 175279",   "https://listen.radioking.com/radio/175279/stream/216784" },
    { "Radio Caroline",      "http://sc6.radiocaroline.net:8040/stream"         },
    { "Raute Musik Club",    "https://club-high.rautemusik.fm/;"                },
    { "WGMC Jazz",           "http://greece-media.monroe.edu/wgmc.mp3"          },
    { "Radio Banovina",      "https://audio.radio-banovina.hr:9998/;"           },
    { "Radio Paradise",      "http://stream.radioparadise.com/mp3-128"          },
};

inline constexpr int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

} // namespace stationsDefaults
