// Persistent storage. Today: ESP32 NVS (Preferences) for small key/value
// settings. Tomorrow: SD card for stations, recordings, themes, MP3 files.
//
// WiFi credentials live in the "wifi" NVS namespace as an ordered list:
//   n         : int, number of saved networks (0..kWifiMaxNetworks)
//   s0..s9    : string, SSID for slot i
//   p0..p9    : string, password for slot i
// Legacy single-cred keys (`ssid`, `pass`) migrate to slot 0 on first load.

#pragma once

#include <Arduino.h>

// ---- NVS (Preferences): generic typed access ----------------------------
String   storageGetString(const char *ns, const char *key, const String &dflt = "");
bool     storagePutString(const char *ns, const char *key, const String &value);
int32_t  storageGetInt(const char *ns, const char *key, int32_t dflt = 0);
bool     storagePutInt(const char *ns, const char *key, int32_t value);
bool     storageRemove(const char *ns, const char *key);

// ---- WiFi credential list ------------------------------------------------
constexpr int kWifiMaxNetworks = 10;

// Load the list from NVS into the in-memory cache. Migrates legacy single-
// credential keys (`wifi/ssid`, `wifi/pass`) into slot 0 on first call.
// Safe to call multiple times.
void        wifiLoadNetworks();

int         wifiNetworkCount();
bool        wifiHasNetworks();
const char *wifiNetworkSsid(int idx);
const char *wifiNetworkPass(int idx);

// Append. Returns false if the list is full, SSID is empty, or the same
// SSID already exists (in which case the password is updated in-place).
bool        wifiAddNetwork(const String &ssid, const String &pass);
bool        wifiRemoveNetwork(int idx);
bool        wifiMoveNetwork(int from, int to);
void        wifiClearAllNetworks();

// ---- SD card -------------------------------------------------------------
// Assumes the pin assignments from Volos's original sketch:
//   clk=16 cmd=15 d0=17 d1=18 d2=13 d3=14 (SD_MMC 4-bit).
bool     storageSdMount();
bool     storageSdMounted();
void     storageSdUnmount();

// Read a whole UTF-8 text file into a String. Returns false on any error
// or if SD isn't mounted. Silently truncates at maxBytes.
bool     storageSdReadText(const char *path, String &out, size_t maxBytes = 8192);

// Write a whole String as text (overwrite). Returns false on any error.
bool     storageSdWriteText(const char *path, const String &content);

// Simple presence check.
bool     storageSdExists(const char *path);
