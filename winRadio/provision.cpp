#include "provision.h"
#include "config.h"
#include "storage.h"
#include "net.h"

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

namespace {
WebServer s_http(80);
DNSServer s_dns;
bool      s_active = false;

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
    String p; p.reserve(2560);
    p += F("<!doctype html><html><head><meta charset=utf-8>"
           "<meta name=viewport content='width=device-width,initial-scale=1'>"
           "<title>Waveshare Internet Radio &mdash; Setup</title>"
           "<style>body{font-family:sans-serif;max-width:520px;margin:1em auto;padding:0 1em;color:#222}"
           "h1{font-size:1.15em}"
           ".net{padding:.4em .25em;border-bottom:1px solid #ddd;cursor:pointer}"
           ".net:hover{background:#eef}"
           ".rssi{color:#888;font-size:.85em;float:right}"
           "input,button{width:100%;box-sizing:border-box;padding:.55em;font-size:1em;margin:.25em 0}"
           "button{background:#18c;color:#fff;border:0;padding:.8em;border-radius:4px}"
           "label{display:block;margin-top:.5em}</style></head><body>");
    p += F("<h1>Waveshare Internet Radio &mdash; WiFi setup</h1>"
           "<p>Tap a network (or type one) and enter the password.</p>"
           "<div id=nets>");
    int n = netScanCount();
    if (n == 0) {
        p += F("<p><em>No networks found &mdash; <a href=/rescan>rescan</a>.</em></p>");
    } else {
        for (int i = 0; i < n; i++) {
            const ScanResult *r = netScanResult(i);
            if (!r) continue;
            p += F("<div class=net onclick=\"document.getElementById('ssid').value="
                   "this.getAttribute('data-ssid')\" data-ssid=\"");
            p += htmlEscape(r->ssid);
            p += F("\">");
            p += htmlEscape(r->ssid);
            p += F("<span class=rssi>");
            p += r->rssi;
            p += F(" dBm</span></div>");
        }
    }
    p += F("</div>"
           "<form method=POST action=/save autocomplete=off>"
           "<label>SSID <input id=ssid name=ssid required></label>"
           "<label>Password <input name=pass type=password></label>"
           "<button type=submit>Save &amp; reconnect</button>"
           "</form>"
           "<p><a href=/rescan>Rescan networks</a></p>"
           "</body></html>");
    return p;
}

void handleIndex() { s_http.send(200, "text/html", renderIndex()); }

void handleSave() {
    String ssid = s_http.arg("ssid"); ssid.trim();
    String pass = s_http.arg("pass");
    if (ssid.length() == 0) { s_http.send(400, "text/plain", "Empty SSID"); return; }
    if (!wifiAddNetwork(ssid, pass)) {
        s_http.send(500, "text/plain",
            "Save failed (list full? remove one first)."); return;
    }
    String ok;
    ok += F("<!doctype html><html><body style='font-family:sans-serif;max-width:520px;margin:1em auto;padding:0 1em'>");
    ok += F("<h2>Saved.</h2><p>Added <code>");
    ok += htmlEscape(ssid);
    ok += F("</code> to the saved-networks list.</p>");
    ok += F("<p>The radio will now try to join it. You can close this tab.</p>");
    ok += F("</body></html>");
    s_http.send(200, "text/html", ok);
    Serial.printf("provision: saved '%s'\r\n", ssid.c_str());
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

void provisionStart() {
    if (s_active) return;
    WiFi.disconnect(false, true);
    WiFi.mode(WIFI_AP);
    const char *pass = strlen(PROVISION_AP_PASS) ? PROVISION_AP_PASS : nullptr;
    WiFi.softAP(PROVISION_AP_SSID, pass);
    IPAddress ip = WiFi.softAPIP();

    s_dns.setErrorReplyCode(DNSReplyCode::NoError);
    s_dns.start(53, "*", ip);

    s_http.on("/",        HTTP_GET,  handleIndex);
    s_http.on("/save",    HTTP_POST, handleSave);
    s_http.on("/rescan",  HTTP_GET,  handleRescan);
    s_http.onNotFound(handleCaptive);
    s_http.begin();

    s_active = true;
    Serial.printf("provision: AP '%s' up at http://%s\r\n",
                  PROVISION_AP_SSID, ip.toString().c_str());
}

void provisionPoll() {
    if (!s_active) return;
    s_dns.processNextRequest();
    s_http.handleClient();
}

void provisionStop() {
    if (!s_active) return;
    s_http.stop();
    s_dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    s_active = false;
    Serial.println("provision: AP stopped");
}

bool   provisionActive() { return s_active; }
String provisionApIp()   { return s_active ? WiFi.softAPIP().toString() : String(); }
