#include "net.h"
#include "config.h"
#include "storage.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <algorithm>
#include <vector>

namespace {
const char *s_hostname = DEFAULT_HOSTNAME;
std::vector<ScanResult> s_scan;
int  s_lastJoinedSlot = -1;

// Insert or update the SSID entry keeping the strongest RSSI.
void upsertScan(const String &ssid, int32_t rssi, uint8_t enc, uint8_t channel) {
    if (ssid.length() == 0) return;
    for (auto &r : s_scan) {
        if (r.ssid == ssid) {
            if (rssi > r.rssi) { r.rssi = rssi; r.encryption = enc; r.channel = channel; }
            return;
        }
    }
    s_scan.push_back({ssid, rssi, enc, channel});
}

// Ordered insertion: try this network for up to timeoutMs. Polls abortCb
// frequently so the caller can break out. Returns true if it joined.
bool tryJoin(const char *ssid, const char *pass, uint32_t timeoutMs,
             bool (*abortCb)()) {
    WiFi.disconnect(false, true);  // clear previous config without erasing creds
    delay(50);
    WiFi.begin(ssid, pass);
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        if (WiFi.status() == WL_CONNECTED) return true;
        if (abortCb && abortCb()) return false;
        delay(100);
    }
    return false;
}
} // namespace

void netBegin() {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(s_hostname);

    // Unlock the full 2.4 GHz band (channels 1..13) so APs hosted on 12 /
    // 13 are visible. "01" is the ITU world/global regulatory code and is
    // accepted by ESP-IDF. Without this, Android hotspots that auto-pick
    // channel 12 or 13 are simply invisible to our scan.
    wifi_country_t cc = {};
    strcpy(cc.cc, "01");
    cc.schan        = 1;
    cc.nchan        = 13;
    cc.policy       = WIFI_COUNTRY_POLICY_MANUAL;
    esp_wifi_set_country(&cc);

    // Protected Management Frames: mark ourselves as capable (but not
    // required) so WPA3 / WPA2-with-PMF APs -- such as the Pixel hotspot
    // when "Hotspot security" is set to WPA3-Personal -- will let us
    // associate. WPA2-only APs ignore the flag.
    wifi_config_t sta = {};
    esp_wifi_get_config(WIFI_IF_STA, &sta);
    sta.sta.pmf_cfg.capable  = true;
    sta.sta.pmf_cfg.required = false;
    esp_wifi_set_config(WIFI_IF_STA, &sta);
}

bool netConnect(bool (*abortCb)(),
                void (*progressCb)(int, int, const char *)) {
    s_lastJoinedSlot = -1;
    int n = wifiNetworkCount();
    if (n == 0) return false;
    for (int i = 0; i < n; i++) {
        if (abortCb && abortCb()) return false;
        const char *ssid = wifiNetworkSsid(i);
        const char *pass = wifiNetworkPass(i);
        if (progressCb) progressCb(i + 1, n, ssid);
        Serial.printf("net: trying slot %d/%d ssid=%s\r\n", i + 1, n, ssid);
        if (tryJoin(ssid, pass, 8000, abortCb)) {
            s_lastJoinedSlot = i;
            Serial.printf("net: connected to %s (slot %d), ip=%s\r\n",
                          ssid, i, WiFi.localIP().toString().c_str());
            return true;
        }
    }
    return false;
}

void netReconnect() {
    WiFi.disconnect(true);
    delay(100);
    netConnect();
}

int netLastJoinedSlot() { return s_lastJoinedSlot; }

bool   netConnected() { return WiFi.status() == WL_CONNECTED; }
String netLocalIp()   { return WiFi.localIP().toString(); }
int    netRssi()      { return WiFi.RSSI(); }
const char *netHostname() { return s_hostname; }

// ---- Scan ---------------------------------------------------------------

int netScanNow() {
    s_scan.clear();
    WiFi.scanDelete();
    // Active scan (async=false), include hidden SSIDs, all channels, longer
    // per-channel dwell so phone hotspots with slower beacon intervals land
    // in the result. passive=false means we send probe requests, which also
    // prompts APs with cloaked SSIDs to reply.
    int n = WiFi.scanNetworks(/*async=*/false,
                              /*showHidden=*/true,
                              /*passive=*/false,
                              /*max_ms_per_chan=*/300);
    if (n < 0) return 0;
    for (int i = 0; i < n; i++) {
        upsertScan(WiFi.SSID(i), WiFi.RSSI(i), WiFi.encryptionType(i), WiFi.channel(i));
    }
    // Sort by descending RSSI so the strongest networks come first.
    std::sort(s_scan.begin(), s_scan.end(),
              [](const ScanResult &a, const ScanResult &b) {
                  return a.rssi > b.rssi;
              });
    WiFi.scanDelete();
    return (int)s_scan.size();
}

int netScanCount() { return (int)s_scan.size(); }
const ScanResult *netScanResult(int idx) {
    if (idx < 0 || idx >= (int)s_scan.size()) return nullptr;
    return &s_scan[idx];
}

bool netConnectAdhoc(const String &ssid, const String &pass, uint32_t timeoutMs) {
    if (ssid.length() == 0) return false;
    s_lastJoinedSlot = -1;
    Serial.printf("net: ad-hoc connect to '%s'...\r\n", ssid.c_str());
    return tryJoin(ssid.c_str(), pass.c_str(), timeoutMs, nullptr);
}

// ---- mDNS / discovery (stubs) -------------------------------------------
void netStartMdns(const char *)  {}
void netStopMdns()               {}
void netBroadcastPresence()      {}
