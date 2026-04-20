// Audio: ES8311 codec init, ESP32-audioI2S setup, station playback,
// metadata callbacks, and the Morse "R" speaker self-test.
//
// All of the radio* CLI / web entry points eventually land here. State is
// owned by this module; readers go through the accessors below.

#pragma once

#include <Arduino.h>

// ---- Lifecycle -----------------------------------------------------------
// Order matters: audioCodecInit -> audioPlayMorseR (optional self-test)
// -> audioBegin -> audioStartDefault. The morse test must run *before*
// audioBegin claims I2S 0.
bool audioCodecInit();
void audioPlayMorseR();           // dot-dash-dot 700 Hz; safe to skip
bool audioBegin();                // claims I2S, registers event callback
bool audioStartDefault();         // connects to station 0
void audioLoop();                 // call every loop() iteration

// ---- Playback control ----------------------------------------------------
int  audioCurrentStation();       // 0-based index into stations module
bool audioSelectStation(int idx); // true on success
void audioNextStation();
void audioPrevStation();

int  audioVolume();               // 1..5
void audioSetVolume(int v);       // clamped 1..5

// ---- State observed via callbacks ----------------------------------------
const char *audioCurStation();    // station name from ICY (may be empty)
const char *audioSongPlaying();   // currently playing track / stream title
long        audioBitrate();       // kbps; 0 if unknown
bool        audioIsRunning();
unsigned    audioInfoEventCount();// diagnostic: how many evt_info we saw

// ---- Diagnostics -------------------------------------------------------
// When enabled, audio library events are mirrored to Serial. Off by default.
bool audioLogEnabled();
void audioSetLogEnabled(bool on);
