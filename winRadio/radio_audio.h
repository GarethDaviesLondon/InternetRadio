// Audio: ES8311 codec init, ESP32-audioI2S setup, station playback,
// metadata callbacks, and the Morse "R" speaker self-test.
//
// All of the radio* CLI / web entry points eventually land here. State is
// owned by this module; readers go through the accessors below.

#pragma once

#include <Arduino.h>

// ---- Lifecycle -----------------------------------------------------------
// Order matters: audioCodecInit -> audioPlayMorseR (optional self-test)
// -> audioRestoreSession -> audioBegin -> audioStartLast. The morse test
// must run *before* audioBegin claims I2S 0.
bool audioCodecInit();
void audioPlayMorseR();           // legacy name; plays the boot chirp
void audioPlayCwString(const char *text);   // CW any uppercase string at 35 WPM
void audioRestoreSession();       // read last volume + station from NVS
bool audioBegin();                // claims I2S, registers event callback
bool audioStartLast();            // connect to the remembered station
void audioLoop();                 // call every loop() iteration

// Friendly display name for a station slot: last URL path segment, with
// common audio suffixes / bit-rates stripped, or the per-slot NVS
// override name if one is set. Never falls through to the ICY
// broadcast name -- that would make the saved-list UI show the
// currently-playing stream's name in slot 0, which is wrong.
const char *audioStationDisplayName(int idx);

// What's actually playing right now (for the "now playing" card on the
// LCD and web UI). Precedence:
//   1. If audioPlayAdhoc set a name -> that name.
//   2. If the current slot's stream emitted an ICY station name -> that.
//   3. Fall back to audioStationDisplayName(current slot).
// This is the function that used to overload audioStationDisplayName,
// causing slot N to appear as the preview name after /api/listen.
const char *audioNowPlayingName();

// ---- Playback control ----------------------------------------------------
int  audioCurrentStation();       // 0-based index into stations module
bool audioSelectStation(int idx); // true on success

// Update the "currently playing slot" pointer without reconnecting.
// Used by reorder / delete operations that change the index of the
// already-playing entry; the stream stays up, only the bookkeeping
// moves. Persists to NVS.
void audioSetCurrentSlot(int idx);
void audioNextStation();
void audioPrevStation();

// Ad-hoc playback: connect to an arbitrary URL without touching the
// saved-stations list or the "current station index". Used by the web
// /discover page's "Listen" button to preview a candidate before the
// user decides to save it. On the next audioSelectStation or audio-
// generated slot change the override goes away naturally.
bool audioPlayAdhoc(const char *url, const char *name);

int  audioVolume();               // 1..5 bucket (for the on-screen bar)
void audioSetVolume(int v);       // 1..5 "big step" -- maps to raw 4/8/12/16/20

// Pause / resume current playback. audioIsPaused() tracks our intent;
// the underlying library's state may drift after a stream ends.
void audioTogglePause();
bool audioIsPaused();

// Fine-grained volume, used by the web UI slider. 0..21 passes straight to
// ESP32-audioI2S setVolume(). audioVolume() is a rounded-up bucket of this.
int  audioVolumeRaw();
void audioSetVolumeRaw(int raw);

// Three-band tone control (bass, mid, treble). Each in -40..+6 dB.
// Persists to NVS so the saved EQ survives reboots.
void audioSetEq(int8_t bass, int8_t mid, int8_t treble);
int8_t audioEqBass();
int8_t audioEqMid();
int8_t audioEqTreble();

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
