// Preset radio stations. Today: a hardcoded list. Tomorrow: also a set
// loaded from SD (plus per-set switching from the web UI).

#pragma once

int         stationsCount();
const char *stationsUrl(int idx);    // NVS override > SD CSV > compiled preset
const char *stationsName(int idx);   // NVS override > SD CSV > compiled preset

// Per-slot edits persisted in NVS (namespace "stations", keys na<i> / ur<i>).
// Empty string clears that field. Returns true on persistent write success;
// on "out of space" errors it progressively drops the highest-indexed other
// override and retries, so slot `idx` always wins.
bool        stationsSetSlot(int idx, const String &name, const String &url);
void        stationsResetSlot(int idx);                 // clear both fields
void        stationsResetAllOverrides();

// Direct access to the override layer (empty String if none).
const char *stationsOverrideName(int idx);
const char *stationsOverrideUrl(int idx);

// Load / reload overrides from NVS. Called once at boot, after SD is
// mounted and stationsLoadFromSd() has run.
void        stationsApplyOverrides();

// Replace the active list with a CSV read from SD. Each non-comment line
// is "Friendly name,url" (whitespace-trimmed). If fewer than 1 station is
// parsed the hardcoded presets are kept. Returns the number of stations
// now active (>= 1 on success).
int         stationsLoadFromSd(const char *path = "/stations.csv");

// Revert to the compiled-in preset list (e.g. if SD is pulled later).
void        stationsUseDefaults();
