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
const char *on8citFaviconSvg();

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
           "<link rel=icon type=image/svg+xml href=/favicon.svg>"
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
           "</div>");

    // Saved-networks card. The boot loop walks this list; the user
    // can reorder (the topmost is tried first), forget a stale entry,
    // or hit "Abort current attempt" to break out of an in-progress
    // connect when the AP they really want is further down.
    int sn = wifiNetworkCount();
    if (sn > 0) {
        p += F("<div class=card><h2>Saved networks</h2>"
               "<p style='color:#8aa;font-size:.85em;margin:.1em 0 .6em'>"
               "Boot tries these in order (top first). Use the arrows "
               "to reorder, X to forget.</p>");
        for (int i = 0; i < sn; i++) {
            p += F("<div class=stationRow>"
                   "<div class=station style='flex:1;cursor:default'>"
                   "<strong>");
            p += (i + 1); p += F(".</strong> ");
            p += htmlEscape(wifiNetworkSsid(i));
            const char *pp = wifiNetworkPass(i);
            if (strlen(pp) == 0) p += F("  <small>(open)</small>");
            else                  p += F("  <small>(saved password)</small>");
            p += F("</div>");
            // Up button: disabled at index 0.
            p += F("<form method=POST action=/wifi-up style='display:inline'>"
                   "<input type=hidden name=idx value=");
            p += i; p += F(">");
            p += F("<button class=infoBtn type=submit");
            if (i == 0) p += F(" disabled");
            p += F(" title='Move up'>&#9650;</button></form>");
            // Down button: disabled at last index.
            p += F("<form method=POST action=/wifi-down style='display:inline'>"
                   "<input type=hidden name=idx value=");
            p += i; p += F(">");
            p += F("<button class=infoBtn type=submit");
            if (i == sn - 1) p += F(" disabled");
            p += F(" title='Move down'>&#9660;</button></form>");
            p += F("<form method=POST action=/wifi-del style='display:inline' "
                   "onsubmit=\"return confirm('Forget this network?')\">"
                   "<input type=hidden name=idx value=");
            p += i; p += F(">");
            p += F("<button class=warn type=submit>&#10005;</button></form>"
                   "</div>");
        }
        // Abort button: only meaningful while a boot connect loop is
        // running, but harmless otherwise (the flag just stays cleared).
        p += F("<p style='margin-top:.7em'>"
               "<form method=POST action=/abort style='display:inline'>"
               "<button class=warn type=submit>&#9888; Abort current "
               "connect attempt</button></form>"
               "</p></div>");
    }

    p += F("</body></html>");
    return p;
}

void handleIndex() { s_http.send(200, "text/html", renderIndex()); }

// Rebooting from inside handleSave would preempt the HTTP response. Latch a
// flag instead and let provisionPoll() restart once the response has flushed.
bool s_rebootPending = false;
uint32_t s_rebootAtMs = 0;

// Set by /abort. The boot-time connect loop polls
// provisionAbortRequested() via its abortCb so the loop can break
// out and the user lands back in setup mode where they can pick a
// different network or reorder the list.
bool s_abortRequested = false;

void handleSave() {
    String ssid = s_http.arg("ssid"); ssid.trim();
    String pass = s_http.arg("pass");
    if (ssid.length() == 0) { s_http.send(400, "text/plain", "Empty SSID"); return; }
    if (!wifiAddNetwork(ssid, pass)) {
        s_http.send(500, "text/plain",
            "Save failed (list full? remove one first)."); return;
    }
    // Promote: the SSID the user just typed in is what they want to
    // join NOW, so it should win on the next boot too.
    for (int i = 0; i < wifiNetworkCount(); i++) {
        if (ssid.equalsIgnoreCase(wifiNetworkSsid(i))) {
            wifiPromoteNetwork(i);
            break;
        }
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

// Forget a saved network. Reachable from the AP-portal "Saved
// networks" card; lets the user wipe stale credentials so the boot
// loop won't auto-rejoin one that's no longer wanted.
void handleWifiDel() {
    if (!s_http.hasArg("idx")) { s_http.send(400, "text/plain", "missing idx"); return; }
    int idx = s_http.arg("idx").toInt();
    if (!wifiRemoveNetwork(idx)) { s_http.send(400, "text/plain", "out of range"); return; }
    s_http.sendHeader("Location", "/");
    s_http.send(302);
}

// Reorder a saved network up one slot. (idx -> idx-1)
void handleWifiUp() {
    if (!s_http.hasArg("idx")) { s_http.send(400, "text/plain", "missing idx"); return; }
    int idx = s_http.arg("idx").toInt();
    if (idx > 0) wifiMoveNetwork(idx, idx - 1);
    s_http.sendHeader("Location", "/");
    s_http.send(302);
}

// Reorder a saved network down one slot. (idx -> idx+1)
void handleWifiDown() {
    if (!s_http.hasArg("idx")) { s_http.send(400, "text/plain", "missing idx"); return; }
    int idx = s_http.arg("idx").toInt();
    if (idx < wifiNetworkCount() - 1) wifiMoveNetwork(idx, idx + 1);
    s_http.sendHeader("Location", "/");
    s_http.send(302);
}

// Latch the abort flag. The boot-time netConnect() loop's
// abortCb (netAbortOnRightButton in winRadio.ino) polls
// provisionAbortRequested() and breaks out, dropping the user
// back into setup mode where they can pick a different network
// or reorder the list and try again.
void handleAbort() {
    s_abortRequested = true;
    String ok;
    ok += F("<!doctype html><html><head><meta charset=utf-8>"
           "<meta http-equiv='refresh' content='2;url=/'>"
           "<title>ON8CIT WebRadio &mdash; aborting</title>"
           "<style>");
    ok += on8citPageCss();
    ok += F("</style></head><body><header>");
    ok += on8citLogoSvg();
    ok += F("<h1><span class=yel>ON8CIT</span> <span class=cya>WebRadio</span></h1></header>"
           "<div class=card><h2>Abort requested</h2>"
           "<p>The boot connect attempt will stop on the next poll. "
           "Setup mode will resume so you can pick a different network "
           "or reorder the saved list.</p></div></body></html>");
    s_http.send(200, "text/html", ok);
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

void handleFavicon() {
    s_http.sendHeader("Cache-Control", "max-age=86400");
    s_http.send(200, "image/svg+xml", on8citFaviconSvg());
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

    s_http.on("/",            HTTP_GET,  handleIndex);
    s_http.on("/favicon.svg", HTTP_GET,  handleFavicon);
    s_http.on("/favicon.ico", HTTP_GET,  handleFavicon);
    s_http.on("/save",      HTTP_POST, handleSave);
    s_http.on("/wifi-del",  HTTP_POST, handleWifiDel);
    s_http.on("/wifi-up",   HTTP_POST, handleWifiUp);
    s_http.on("/wifi-down", HTTP_POST, handleWifiDown);
    s_http.on("/abort",     HTTP_POST, handleAbort);
    s_http.on("/rescan",    HTTP_GET,  handleRescan);
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

bool   provisionAbortRequested() { return s_abortRequested; }
void   provisionClearAbort()     { s_abortRequested = false; }
