#include "net.h"
#include "config.h"
#include "storage.h"

#include <WiFi.h>
#include <WiFiMulti.h>

static WiFiMulti s_multi;
static const char *s_hostname = DEFAULT_HOSTNAME;

void netBegin() {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(s_hostname);
}

bool netConnect() {
    if (!storageHasWifiCreds()) return false;
    s_multi.addAP(storageWifiSsid(), storageWifiPassword());
    s_multi.run();
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect(true);
        s_multi.run();
    }
    return WiFi.status() == WL_CONNECTED;
}

void netReconnect() {
    WiFi.disconnect(true);
    delay(100);
    if (storageHasWifiCreds()) {
        s_multi.addAP(storageWifiSsid(), storageWifiPassword());
        s_multi.run();
    }
}

bool   netConnected() { return WiFi.status() == WL_CONNECTED; }
String netLocalIp()   { return WiFi.localIP().toString(); }
int    netRssi()      { return WiFi.RSSI(); }
const char *netHostname() { return s_hostname; }

// ---- mDNS / discovery (stubs) -------------------------------------------
// TODO(mdns): #include <ESPmDNS.h>; MDNS.begin(hostname); MDNS.addService(
//   "http", "tcp", 80); also add a TXT record with firmware version and
//   features so a desktop helper can identify radios.
void netStartMdns(const char *) {}
void netStopMdns() {}

// TODO(broadcast): if mDNS proves unreliable on some routers, send a UDP
// broadcast on a fixed port (e.g. 13321) every 30 s with a small JSON
// blob {host, ip, version, station} so a LAN helper can find us.
void netBroadcastPresence() {}
