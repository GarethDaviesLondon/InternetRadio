// Persistent storage. Today: ESP32 NVS (Preferences) for small key/value
// settings. Tomorrow: SD card for stations, recordings, themes, MP3 files.
//
// Naming: the `storage*` family is for the NVS / SD layer itself. Higher
// modules (audio, net) wrap it with domain-specific accessors so they don't
// leak the underlying namespace strings everywhere.

#pragma once

#include <Arduino.h>

// ---- NVS (Preferences) ---------------------------------------------------
// Generic typed key access in a named namespace. Returns the supplied
// default on miss. All calls are thread-safe (Preferences serialises
// internally) but not reentrant within one namespace.
String   storageGetString(const char *ns, const char *key, const String &dflt = "");
bool     storagePutString(const char *ns, const char *key, const String &value);
int32_t  storageGetInt(const char *ns, const char *key, int32_t dflt = 0);
bool     storagePutInt(const char *ns, const char *key, int32_t value);
bool     storageRemove(const char *ns, const char *key);

// Convenience wrappers for the WiFi credential pair used by net + provision.
bool     storageLoadWifiCreds();      // populates getWifiSsid/Password
bool     storageHasWifiCreds();
const char *storageWifiSsid();
const char *storageWifiPassword();
void     storageSaveWifiCreds(const String &ssid, const String &pass);
void     storageClearWifiCreds();

// ---- SD card (stub for now) ----------------------------------------------
// The Waveshare board exposes SD over SDIO on GPIOs 13-18 (matching Volos's
// pin assignments). Full implementation lands when SD work begins.
bool     storageSdMount();            // returns false until implemented
bool     storageSdMounted();
void     storageSdUnmount();
// Future helpers: storageSdSaveStations(...), storageSdLoadStations(...),
// storageSdRecordingPath(...), storageSdMp3List(...), storageSdLoadTheme(...)
