// Web interface (stub).
//
// Future implementation outline:
//   - ESPAsyncWebServer + AsyncWebSocket on port 80
//   - Static SPA served from PROGMEM (small) or LittleFS / SD (themed)
//   - JSON POST endpoints for: /api/wifi, /api/station, /api/volume,
//     /api/recording, /api/sleep
//   - WebSocket /ws pushes state updates: {evt, station, song, bitrate,
//     volume, vbat, rssi}
//   - Recommend pinning the AsyncTCP task to Core 0 alongside WiFi
//
// For now everything below is a no-op so we can wire in the call sites.

#pragma once

void webBegin();        // start server (no-op today)
void webPoll();         // optional housekeeping (no-op today)
void webStop();
bool webRunning();
