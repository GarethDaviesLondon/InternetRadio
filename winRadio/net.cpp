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

const char *encName(uint8_t e) {
    switch (e) {
        case WIFI_AUTH_OPEN:           return "OPEN";
        case WIFI_AUTH_WEP:            return "WEP";
        case WIFI_AUTH_WPA_PSK:        return "WPA";
        case WIFI_AUTH_WPA2_PSK:       return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:   return "WPA/2";
        case WIFI_AUTH_WPA2_ENTERPRISE:return "WPA2-EAP";
        case WIFI_AUTH_WPA3_PSK:       return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:  return "WPA2/3";
        default:                       return "?";
    }
}

const char *wlStatusName(int s) {
    switch (s) {
        case WL_IDLE_STATUS:     return "IDLE";
        case WL_NO_SSID_AVAIL:   return "NO_SSID_AVAIL";
        case WL_SCAN_COMPLETED:  return "SCAN_DONE";
        case WL_CONNECTED:       return "CONNECTED";
        case WL_CONNECT_FAILED:  return "CONNECT_FAILED";
        case WL_CONNECTION_LOST: return "CONNECTION_LOST";
        case WL_DISCONNECTED:    return "DISCONNECTED";
        default:                 return "?";
    }
}

// Snapshot of the most recent disconnect, populated by the event hook.
volatile uint16_t s_lastDisconnectReason = 0;
const char *disconnectReasonName(uint16_t r) {
    // Most informative members of WIFI_REASON_*. Keep brief.
    switch (r) {
        case WIFI_REASON_AUTH_EXPIRE:           return "AUTH_EXPIRE";
        case WIFI_REASON_AUTH_LEAVE:            return "AUTH_LEAVE";
        case WIFI_REASON_ASSOC_EXPIRE:          return "ASSOC_EXPIRE";
        case WIFI_REASON_ASSOC_TOOMANY:         return "ASSOC_TOOMANY";
        case WIFI_REASON_NOT_AUTHED:            return "NOT_AUTHED";
        case WIFI_REASON_NOT_ASSOCED:           return "NOT_ASSOCED";
        case WIFI_REASON_ASSOC_LEAVE:           return "ASSOC_LEAVE";
        case WIFI_REASON_ASSOC_NOT_AUTHED:      return "ASSOC_NOT_AUTHED";
        case WIFI_REASON_DISASSOC_PWRCAP_BAD:   return "PWRCAP_BAD";
        case WIFI_REASON_DISASSOC_SUPCHAN_BAD:  return "SUPCHAN_BAD";
        case WIFI_REASON_IE_INVALID:            return "IE_INVALID";
        case WIFI_REASON_MIC_FAILURE:           return "MIC_FAILURE";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:return "4WAY_HANDSHAKE_TIMEOUT";
        case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT: return "GROUP_KEY_UPDATE_TIMEOUT";
        case WIFI_REASON_IE_IN_4WAY_DIFFERS:    return "IE_IN_4WAY_DIFFERS";
        case WIFI_REASON_GROUP_CIPHER_INVALID:  return "GROUP_CIPHER_INVALID";
        case WIFI_REASON_PAIRWISE_CIPHER_INVALID:return "PAIRWISE_CIPHER_INVALID";
        case WIFI_REASON_AKMP_INVALID:          return "AKMP_INVALID";
        case WIFI_REASON_UNSUPP_RSN_IE_VERSION: return "UNSUPP_RSN_IE_VERSION";
        case WIFI_REASON_INVALID_RSN_IE_CAP:    return "INVALID_RSN_IE_CAP";
        case WIFI_REASON_802_1X_AUTH_FAILED:    return "802_1X_AUTH_FAILED";
        case WIFI_REASON_CIPHER_SUITE_REJECTED: return "CIPHER_SUITE_REJECTED";
        case WIFI_REASON_BEACON_TIMEOUT:        return "BEACON_TIMEOUT";
        case WIFI_REASON_NO_AP_FOUND:           return "NO_AP_FOUND";
        case WIFI_REASON_AUTH_FAIL:             return "AUTH_FAIL";
        case WIFI_REASON_ASSOC_FAIL:            return "ASSOC_FAIL";
        case WIFI_REASON_HANDSHAKE_TIMEOUT:     return "HANDSHAKE_TIMEOUT";
        case WIFI_REASON_CONNECTION_FAIL:       return "CONNECTION_FAIL";
        default:                                return "?";
    }
}

void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_CONNECTED: {
            Serial.printf("wifi: STA_CONNECTED ssid='%.*s' ch=%u bssid=%02x:%02x:%02x:%02x:%02x:%02x\r\n",
                info.wifi_sta_connected.ssid_len, info.wifi_sta_connected.ssid,
                info.wifi_sta_connected.channel,
                info.wifi_sta_connected.bssid[0], info.wifi_sta_connected.bssid[1],
                info.wifi_sta_connected.bssid[2], info.wifi_sta_connected.bssid[3],
                info.wifi_sta_connected.bssid[4], info.wifi_sta_connected.bssid[5]);
            break;
        }
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
            uint16_t r = info.wifi_sta_disconnected.reason;
            s_lastDisconnectReason = r;
            Serial.printf("wifi: STA_DISCONNECTED reason=%u (%s)\r\n", r, disconnectReasonName(r));
            break;
        }
        case ARDUINO_EVENT_WIFI_STA_GOT_IP: {
            Serial.printf("wifi: GOT_IP %s\r\n", IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
            break;
        }
        case ARDUINO_EVENT_WIFI_STA_LOST_IP:
            Serial.println("wifi: LOST_IP");
            break;
        case ARDUINO_EVENT_WIFI_SCAN_DONE:
            Serial.printf("wifi: SCAN_DONE status=%u count=%u\r\n",
                          (unsigned)info.wifi_scan_done.status,
                          (unsigned)info.wifi_scan_done.number);
            break;
        default: break;
    }
}

const ScanResult *findInScan(const char *ssid) {
    for (auto &r : s_scan) if (r.ssid == ssid) return &r;
    return nullptr;
}

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
    s_lastDisconnectReason = 0;
    WiFi.begin(ssid, pass);
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        wl_status_t st = WiFi.status();
        if (st == WL_CONNECTED) {
            Serial.printf("net: joined '%s' in %lu ms\r\n", ssid, (unsigned long)(millis() - start));
            return true;
        }
        if (abortCb && abortCb()) {
            Serial.printf("net: tryJoin '%s' aborted by user (status=%s)\r\n",
                          ssid, wlStatusName(st));
            return false;
        }
        delay(100);
    }
    Serial.printf("net: tryJoin '%s' timed out (status=%s, last reason=%u %s)\r\n",
                  ssid, wlStatusName(WiFi.status()),
                  (unsigned)s_lastDisconnectReason,
                  disconnectReasonName(s_lastDisconnectReason));
    return false;
}
} // namespace

void netBegin() {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(s_hostname);
    WiFi.onEvent(onWifiEvent);  // logs every STA event with reason codes

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

    // PMF (Protected Management Frames) is capable-by-default in
    // arduino-esp32 v3.x, which is what WPA3 / Pixel hotspots want.
    // A previous build reached into esp_wifi_get_config / set_config to
    // flip it explicitly; that turned out to leave the STA in a state
    // where scanNetworks() returned 0. Don't touch it -- the default is
    // already right.
}

bool netConnect(bool (*abortCb)(),
                void (*progressCb)(int, int, const char *)) {
    s_lastJoinedSlot = -1;
    int n = wifiNetworkCount();
    if (n == 0) return false;

    // Build the attempt order: last-known-good slot first (if still valid),
    // then the remaining slots in their saved order. We keep the persistent
    // slot list unchanged -- only the *try* order is re-ranked.
    int lastGood = storageGetInt("radio", "lastwifi", -1);
    if (lastGood >= n) lastGood = -1;
    int order[kWifiMaxNetworks];
    int m = 0;
    if (lastGood >= 0) order[m++] = lastGood;
    for (int i = 0; i < n; i++) if (i != lastGood) order[m++] = i;

    for (int a = 0; a < m; a++) {
        int i = order[a];
        if (abortCb && abortCb()) return false;
        const char *ssid = wifiNetworkSsid(i);
        const char *pass = wifiNetworkPass(i);
        if (progressCb) progressCb(a + 1, m, ssid);
        // Cross-reference the saved SSID against the latest scan so we can
        // see at a glance whether the AP was even visible at scan time.
        const ScanResult *sr = findInScan(ssid);
        if (sr) {
            Serial.printf("net: trying slot %d/%d ssid='%s' (scan: ch%u %ddBm %s)\r\n",
                          i + 1, n, ssid, sr->channel, (int)sr->rssi,
                          encName(sr->encryption));
        } else {
            Serial.printf("net: trying slot %d/%d ssid='%s' (NOT in latest scan)\r\n",
                          i + 1, n, ssid);
        }
        if (tryJoin(ssid, pass, 8000, abortCb)) {
            s_lastJoinedSlot = i;
            storagePutInt("radio", "lastwifi", i);  // remember across reboots
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
    Serial.printf("net: scan found %d network(s) (raw=%d)\r\n",
                  (int)s_scan.size(), n);
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

// ---- Diagnostics --------------------------------------------------------

const char *netEncName(uint8_t enc) { return encName(enc); }

void netPrintDiag() {
    Serial.println();
    Serial.print("Mode      : ");
    switch (WiFi.getMode()) {
        case WIFI_OFF:     Serial.println("OFF");    break;
        case WIFI_STA:     Serial.println("STA");    break;
        case WIFI_AP:      Serial.println("AP");     break;
        case WIFI_AP_STA:  Serial.println("AP+STA"); break;
        default:           Serial.println("?");      break;
    }
    Serial.print("Hostname  : "); Serial.println(s_hostname);
    Serial.print("MAC (STA) : "); Serial.println(WiFi.macAddress());
    Serial.print("MAC (AP)  : "); Serial.println(WiFi.softAPmacAddress());
    int st = WiFi.status();
    Serial.printf("Status    : %d (%s)\r\n", st, wlStatusName(st));
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("SSID      : "); Serial.println(WiFi.SSID());
        Serial.print("BSSID     : "); Serial.println(WiFi.BSSIDstr());
        Serial.print("Channel   : "); Serial.println(WiFi.channel());
        Serial.print("RSSI      : "); Serial.print(WiFi.RSSI()); Serial.println(" dBm");
        Serial.print("IP        : "); Serial.println(WiFi.localIP().toString());
        Serial.print("Gateway   : "); Serial.println(WiFi.gatewayIP().toString());
        Serial.print("DNS       : "); Serial.println(WiFi.dnsIP().toString());
    }
    Serial.printf("Last disc : %u (%s)\r\n",
                  (unsigned)s_lastDisconnectReason,
                  disconnectReasonName(s_lastDisconnectReason));
    wifi_country_t cc = {};
    if (esp_wifi_get_country(&cc) == ESP_OK) {
        Serial.printf("Country   : %.3s ch%u..%u policy=%d\r\n",
                      cc.cc, cc.schan, (unsigned)(cc.schan + cc.nchan - 1),
                      (int)cc.policy);
    }
    Serial.printf("Scan cache: %d network(s)\r\n", (int)s_scan.size());
    for (auto &r : s_scan) {
        Serial.printf("  ch%-2u  %4d dBm  %-7s  %s\r\n",
                      r.channel, (int)r.rssi, encName(r.encryption), r.ssid.c_str());
    }
    Serial.println();
}

// ---- mDNS / discovery (stubs) -------------------------------------------
void netStartMdns(const char *)  {}
void netStopMdns()               {}
void netBroadcastPresence()      {}
