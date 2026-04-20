#pragma once

#include <Arduino.h>

// ---- NVS-backed WiFi credential storage ----------------------------------
bool        loadWifiCreds();
bool        hasWifiCreds();
const char *getWifiSsid();
const char *getWifiPassword();

// ---- Serial CLI (9600 8N1, UART0) ----------------------------------------
void cliBegin();            // banner + prompt
void cliFirstRunSetup();    // blocking: prompt for creds if NVS is empty
void cliPoll();             // non-blocking, call from loop()

// ---- Audio library event logging -----------------------------------------
// When enabled, the ESP32-audioI2S callbacks (info / id3 / station / title /
// bitrate) print their payload to Serial with a `[audio]` prefix so you can
// watch what the library is doing. Off by default so the CLI stays clean.
bool audioLogEnabled();
void setAudioLogEnabled(bool on);
// The CLI calls these so it doesn't need direct access to the audio object
// or the sketch's globals.
int         radioStationCount();
const char *radioStationUrl(int idx);
int         radioCurrentStation();
void        radioSelectStation(int idx);   // 0-based; clamped
void        radioNextStation();
void        radioPrevStation();
int         radioVolume();                 // 1..5
void        radioSetVolume(int v);         // 1..5, clamped
long        radioBitrate();
float       radioBattery();
const char *radioSongPlaying();
void        radioReconnectWifi();
void        radioDeepSleep();
