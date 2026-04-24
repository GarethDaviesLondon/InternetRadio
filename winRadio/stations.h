// Preset radio stations. The list is a variable-length array held in NVS,
// 0..stationsMax() entries, editable via "Add" / "Edit" / "Delete" from the
// web UI, the CLI, or the /discover page.
//
// Storage model (namespace "stations"):
//   cnt    uint8   number of active entries
//   na<i>  string  friendly name for entry i  (0..cnt-1)
//   ur<i>  string  stream URL    for entry i  (0..cnt-1)
//
// On first boot (cnt not set), the list is seeded from either
// /stations.csv on the SD card, or from the compiled-in defaults.

#pragma once

#include <Arduino.h>

// Hard upper bound on slot count. 30 stations fits comfortably in RAM
// (~7 KB for the String pairs) and NVS (~60 strings @ ~220 B = ~13 KB,
// under the default ~20 KB NVS partition).
int         stationsMax();
int         stationsCount();                 // 0..stationsMax()
const char *stationsName(int idx);
const char *stationsUrl(int idx);

// Boot-time load. Reads from NVS if the list has been populated before.
// Otherwise seeds from /stations.csv (if present on SD) or the compiled
// defaults. Call once in setup() after Wire.begin + SD mount.
void        stationsBegin();

// Edit an existing slot in place. Returns false if idx is out of range
// or NVS is full.
bool        stationsEdit(int idx, const String &name, const String &url);

// Append a new station. Returns the new index, or -1 if the list is
// already at stationsMax() or NVS is full.
int         stationsAdd(const String &name, const String &url);

// Remove a slot. Remaining entries shift down so the list stays
// contiguous. Returns false if idx is out of range.
bool        stationsDelete(int idx);

// Wipe everything and reseed from /stations.csv or the compiled defaults.
void        stationsResetToDefaults();

// Bulk replace from an SD CSV. Each non-comment line is "name,url". Used
// by the CLI "sd reload" command and on first boot if NVS is empty.
// Returns the number of stations now active.
int         stationsLoadFromSd(const char *path = "/stations.csv");
