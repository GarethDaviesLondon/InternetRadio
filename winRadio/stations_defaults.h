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
    {"Mix Megapol", "http://tx-bauerse.sharp-stream.com/http_live.php?ua=WEB&i=mixmegapol_instream_se_mp3"},
    { "Radio Caroline",      "http://sc6.radiocaroline.net:8040/stream"         },
    {"BBC World" ,"https://stream.live.vc.bbcmedia.co.uk/bbc_world_service"},
    {"BSJ","http://64.95.243.43:8002/stream"},
    {"Swiss Jazz", "http://stream.srg-ssr.ch/m/rsj/mp3_128" },
    {"Heart 80s", "https://media-ssl.musicradio.com/Heart80sMP3"},
    {"Gold","https://media-ssl.musicradio.com/GoldMP3"},
    {"Nostaligi","https://media-ssl.musicradio.com/NostalgiaMP3"}
};

inline constexpr int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

} // namespace stationsDefaults
