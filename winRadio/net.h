// Network: WiFi STA connect, RSSI / hostname, scan, mDNS stub.
//
// netConnect() walks the saved-network list (see storage.h) in order and
// returns true as soon as one joins, or false if all attempts fail / time
// out. A caller can hand in an "abort" callback that's polled between
// attempts so a button press (or CLI input) can break out early.

#pragma once

#include <Arduino.h>

void  netBegin();                               // mode + hostname; non-blocking
bool  netConnect(bool (*abortCb)() = nullptr,   // true -> stop trying
                 void (*progressCb)(int slot, int total, const char *ssid) = nullptr);
void  netReconnect();
bool  netConnected();
String netLocalIp();
int   netRssi();
String netCurrentSsid();    // empty when not associated
const char *netHostname();

// The saved-network slot that successfully connected during the last
// netConnect() call (or the latest reconnect). -1 if no connection yet.
// Cleared when netConnect() returns false / when we reconnect.
int   netLastJoinedSlot();

// ---- Scan ---------------------------------------------------------------
struct ScanResult {
    String  ssid;
    int32_t rssi;
    uint8_t encryption;   // WIFI_AUTH_OPEN / WPA / WPA2 / ...
    uint8_t channel;      // 2.4 GHz channel (1..13)
};

// One-shot connect that bypasses the saved-list loop. Used by the CLI's
// `wifi connect <ssid> [pass]` so users can force-try a hotspot that
// doesn't show up in the scan. progressCb is invoked roughly every
// 100 ms with the elapsed milliseconds, so callers can render a
// growing "..." or a connecting screen.
bool netConnectAdhoc(const String &ssid, const String &pass,
                     uint32_t timeoutMs = 10000,
                     void (*progressCb)(uint32_t elapsedMs) = nullptr);

// Synchronous blocking scan. Returns number of APs found; results are
// cached internally and accessed via netScanResult(i). Uniques SSIDs
// (keeps strongest signal of each).
int               netScanNow();
int               netScanCount();
const ScanResult *netScanResult(int idx);   // nullptr if out of range

// ---- Diagnostics --------------------------------------------------------
// Dump driver state (mode, MAC, status, SSID/BSSID/channel/RSSI, IP,
// gateway, DNS, regulatory country/channel range, last disconnect reason)
// to Serial. Called by the CLI `wifi diag`.
void  netPrintDiag();

// Human-readable names for the auth + wl_status + disconnect-reason enums.
// Handy when other modules (cli, web) want to surface these.
const char *netEncName(uint8_t enc);

// ---- mDNS / discovery (stubs) -------------------------------------------
void  netStartMdns(const char *hostname);
void  netStopMdns();
void  netBroadcastPresence();
