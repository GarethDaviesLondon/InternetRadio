#include "net.h"
#include "config.h"
#include "storage.h"

#include <WiFi.h>
#include <algorithm>
#include <vector>

namespace {
const char *s_hostname = DEFAULT_HOSTNAME;
std::vector<ScanResult> s_scan;
int  s_lastJoinedSlot = -1;

// Insert or update the SSID entry keeping the strongest RSSI.
void upsertScan(const String &ssid, int32_t rssi, uint8_t enc) {
    if (ssid.length() == 0) return;
    for (auto &r : s_scan) {
        if (r.ssid == ssid) {
            if (rssi > r.rssi) { r.rssi = rssi; r.encryption = enc; }
            return;
        }
    }
    s_scan.push_back({ssid, rssi, enc});
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
    int n = WiFi.scanNetworks(/*async=*/false, /*showHidden=*/false);
    if (n < 0) return 0;
    for (int i = 0; i < n; i++) {
        upsertScan(WiFi.SSID(i), WiFi.RSSI(i), WiFi.encryptionType(i));
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

// ---- mDNS / discovery (stubs) -------------------------------------------
void netStartMdns(const char *)  {}
void netStopMdns()               {}
void netBroadcastPresence()      {}
