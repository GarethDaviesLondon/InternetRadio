// Preset radio stations. Today: a hardcoded list. Tomorrow: also a set
// loaded from SD (plus per-set switching from the web UI).

#pragma once

int         stationsCount();
const char *stationsUrl(int idx);
const char *stationsName(int idx);   // friendly name, falls back to URL

// Replace the active list with a CSV read from SD. Each non-comment line
// is "Friendly name,url" (whitespace-trimmed). If fewer than 1 station is
// parsed the hardcoded presets are kept. Returns the number of stations
// now active (>= 1 on success).
int         stationsLoadFromSd(const char *path = "/stations.csv");

// Revert to the compiled-in preset list (e.g. if SD is pulled later).
void        stationsUseDefaults();
