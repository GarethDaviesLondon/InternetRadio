// Preset radio stations. Today: a hardcoded list. Tomorrow: also a set
// loaded from SD (plus per-set switching from the web UI).

#pragma once

int         stationsCount();
const char *stationsUrl(int idx);
const char *stationsName(int idx);   // friendly name, falls back to URL

// Future: stationsLoadSetFromSd(const char *path), stationsActiveSet(),
// stationsAddCustom(...). The audio module just calls stationsCount/Url
// and doesn't care where they came from.
