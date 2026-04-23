#include "provision.h"
#include "config.h"
#include "storage.h"
#include "net.h"

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>

// Shared branding (branding.cpp) -- must be declared at file scope, not
// inside the anonymous namespace below, or the linker will look for
// internal-linkage symbols and fail.
const char *on8citPageCss();
const char *on8citLogoSvg();

namespace {
WebServer s_http(80);
DNSServer s_dns;
bool      s_active  = false;
bool      s_mdnsUp  = false;
constexpr const char *kApMdnsHost = "on8cit-setup";  // on8cit-setup.local

String htmlEscape(const String &s) {
    String out; out.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        switch (c) {
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '&':  out += "&amp;";  break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:   out += c;
        }
    }
    return out;
}

String renderIndex() {
    String p; p.reserve(3000);
    p += F("<!doctype html><html><head><meta charset=utf-8>"
           "<meta name=viewport content='width=device-width,initial-scale=1'>"
           "<title>ON8CIT WebRadio &mdash; Setup</title>"
           "<style>");
    p += on8citPageCss();
    p += F("</style></head><body>"
           "<header>");
    p += on8citLogoSvg();
    p += F("<h1><span class=yel>ON8CIT</span> <span class=cya>WebRadio</span></h1>"
           "</header>");
    p += F("<div class=card><h2>WiFi setup</h2>"
           "<p style='margin:.2em 0 .7em;color:#cdd3de'>"
           "Pick a network below (or type one) and enter the password. "
           "The radio will reboot and join on save.</p>"
           "<div id=nets>");
    int n = netScanCount();
    if (n == 0) {
        p += F("<p style='color:#8aa'><em>No networks found &mdash; "
               "<a href=/rescan>rescan</a>.</em></p>");
    } else {
        for (int i = 0; i < n; i++) {
            const ScanResult *r = netScanResult(i);
            if (!r) continue;
            p += F("<div class=net onclick=\"document.getElementById('ssid').value="
                   "this.getAttribute('data-ssid')\" data-ssid=\"");
            p += htmlEscape(r->ssid);
            p += F("\"><span class=rssi>ch");
            p += r->channel;
            p += F(" &middot; ");
            p += r->rssi;
            p += F(" dBm</span>");
            p += htmlEscape(r->ssid);
            p += F("</div>");
        }
    }
    p += F("</div>"
           "<form method=POST action=/save autocomplete=off>"
           "<label>SSID <input id=ssid name=ssid required></label>"
           "<label>Password <input name=pass type=password></label>"
           "<button type=submit>Save &amp; reconnect</button>"
           "</form>"
           "<p style='margin-top:.8em'><a href=/rescan>Rescan networks</a></p>"
           "</div>"
           "</body></html>");
    return p;
}

void handleIndex() { s_http.send(200, "text/html", renderIndex()); }

// Rebooting from inside handleSave would preempt the HTTP response. Latch a
// flag instead and let provisionPoll() restart once the response has flushed.
bool s_rebootPending = false;
uint32_t s_rebootAtMs = 0;

void handleSave() {
    String ssid = s_http.arg("ssid"); ssid.trim();
    String pass = s_http.arg("pass");
    if (ssid.length() == 0) { s_http.send(400, "text/plain", "Empty SSID"); return; }
    if (!wifiAddNetwork(ssid, pass)) {
        s_http.send(500, "text/plain",
            "Save failed (list full? remove one first)."); return;
    }
    String ok;
    ok += F("<!doctype html><html><head><meta charset=utf-8>"
           "<meta http-equiv='refresh' content='8'>"
           "<title>ON8CIT WebRadio &mdash; saved</title>"
           "<style>");
    ok += on8citPageCss();
    ok += F("</style></head><body><header>");
    ok += on8citLogoSvg();
    ok += F("<h1><span class=yel>ON8CIT</span> <span class=cya>WebRadio</span></h1></header>"
           "<div class=card><h2>Saved</h2>"
           "<p>Added <code>");
    ok += htmlEscape(ssid);
    ok += F("</code> to the saved-networks list.</p>"
           "<p><strong>The radio will reboot in a moment</strong> and join the new "
           "network. You can close this tab.</p></div></body></html>");
    s_http.send(200, "text/html", ok);
    Serial.printf("provision: saved '%s', rebooting in 2 s\r\n", ssid.c_str());
    s_rebootPending = true;
    s_rebootAtMs = millis() + 2000;
}

// AP-mode rescan. ESP32-S3 in AP mode can't scan on its own RF chain; switch
// briefly to AP+STA so the scan succeeds, then drop back.
void handleRescan() {
    WiFi.mode(WIFI_AP_STA);
    delay(50);
    netScanNow();
    WiFi.mode(WIFI_AP);
    s_http.sendHeader("Location", "/");
    s_http.send(302);
}

// Catch-all for captive-portal probes (Android / iOS / Windows hit various
// well-known URLs). Redirect everything to the setup page.
void handleCaptive() {
    s_http.sendHeader("Location", "/");
    s_http.send(302);
}
} // namespace

static void provisionStartInternal(bool keepSta) {
    if (s_active) return;
    // keepSta=false: classic setup-mode path. We were called because the
    //   user explicitly wants to reconfigure and there's no point keeping
    //   the STA around. We tear it down so the RF can focus on the AP.
    // keepSta=true : background mode. Runs alongside a live or imminent
    //   STA association attempt, so the portal is reachable *while* the
    //   radio is still trying saved networks. Requires WIFI_AP_STA.
    if (!keepSta) WiFi.disconnect(false, true);
    WiFi.mode(keepSta ? WIFI_AP_STA : WIFI_AP);
    const char *pass = strlen(PROVISION_AP_PASS) ? PROVISION_AP_PASS : nullptr;
    WiFi.softAP(PROVISION_AP_SSID, pass);
    IPAddress ip = WiFi.softAPIP();

    s_dns.setErrorReplyCode(DNSReplyCode::NoError);
    s_dns.start(53, "*", ip);

    // mDNS announcement on the AP so iOS / macOS / Windows devices can
    // open http://on8cit-setup.local directly. The wildcard DNS above
    // already makes any hostname resolve to the AP, so this is mainly a
    // nicer URL to tell the user.
    if (MDNS.begin(kApMdnsHost)) {
        MDNS.addService("http", "tcp", 80);
        s_mdnsUp = true;
        Serial.printf("provision: mDNS up at http://%s.local\r\n", kApMdnsHost);
    }

    s_http.on("/",        HTTP_GET,  handleIndex);
    s_http.on("/save",    HTTP_POST, handleSave);
    s_http.on("/rescan",  HTTP_GET,  handleRescan);
    s_http.onNotFound(handleCaptive);
    s_http.begin();

    s_active = true;
    Serial.printf("provision: AP '%s' up at http://%s (keepSta=%d)\r\n",
                  PROVISION_AP_SSID, ip.toString().c_str(), keepSta ? 1 : 0);
}

void provisionStart()           { provisionStartInternal(/*keepSta=*/false); }
void provisionStartBackground() { provisionStartInternal(/*keepSta=*/true);  }

void provisionPoll() {
    if (!s_active) return;
    s_dns.processNextRequest();
    s_http.handleClient();
    if (s_rebootPending && (int32_t)(millis() - s_rebootAtMs) >= 0) {
        s_http.stop();
        s_dns.stop();
        WiFi.softAPdisconnect(true);
        delay(100);
        ESP.restart();
    }
}

void provisionStop() {
    if (!s_active) return;
    s_http.stop();
    s_dns.stop();
    if (s_mdnsUp) { MDNS.end(); s_mdnsUp = false; }
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    s_active = false;
    Serial.println("provision: AP stopped");
}

bool   provisionActive() { return s_active; }
String provisionApIp()   { return s_active ? WiFi.softAPIP().toString() : String(); }
