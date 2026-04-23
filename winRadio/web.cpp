#include "web.h"
#include "config.h"
#include "radio_audio.h"
#include "stations.h"
#include "storage.h"
#include "net.h"
#include "display.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>

// Shared ON8CIT visual identity (defined in branding.cpp). Declared at
// global (non-anonymous) scope so the linker binds to the external
// symbols; putting them inside the anonymous namespace below would make
// them internal-linkage-only and the link would fail with "used but
// never defined".
const char *on8citPageCss();
const char *on8citLogoSvg();

namespace {
WebServer s_http(80);
bool      s_running = false;
bool      s_mdnsUp  = false;
uint32_t  s_mdnsLastCheckMs = 0;
// ESPmDNS occasionally stops responding after a router reboot or a lease
// expiry. Re-call MDNS.begin() every 30 s while STA is up. MDNS.begin() is
// idempotent (it tears down the previous service record), so calling it
// unconditionally is cheap.
constexpr uint32_t kMdnsHeartbeatMs = 30000;

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

// Shared page chrome + banner so every route keeps the ON8CIT branding.
String pageHead(const char *title) {
    String p;
    p.reserve(2600);
    p += F("<!doctype html><html><head><meta charset=utf-8>"
           "<meta name=viewport content='width=device-width,initial-scale=1'>"
           "<title>");
    p += title;
    p += F("</title><style>");
    p += on8citPageCss();
    p += F("</style></head><body>"
           "<header>");
    p += on8citLogoSvg();
    p += F("<h1><span class=yel>ON8CIT</span> <span class=cya>WebRadio</span></h1>"
           "</header>");
    return p;
}

String pageFoot() {
    String p;
    p += F("<footer>");
    p += FIRMWARE_NAME; p += F(" "); p += FIRMWARE_VERSION;
    p += F(" &mdash; <a href=/>home</a></footer></body></html>");
    return p;
}

// ---- JSON helpers for the API ------------------------------------------
String jsonEscape(const String &s) {
    String out; out.reserve(s.length() + 4);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((uint8_t)c < 0x20) { char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", c); out += buf; }
                else out += c;
        }
    }
    return out;
}

String stateJson() {
    String j; j.reserve(512);
    j += F("{");
    j += F("\"hostname\":\""); j += jsonEscape(netHostname()); j += F("\",");
    j += F("\"ip\":\"");       j += jsonEscape(netLocalIp()); j += F("\",");
    j += F("\"rssi\":");       j += netRssi();                j += F(",");
    j += F("\"ssid\":\"");     j += jsonEscape(WiFi.SSID());  j += F("\",");
    j += F("\"station\":");    j += audioCurrentStation();    j += F(",");
    j += F("\"stations\":");   j += stationsCount();          j += F(",");
    j += F("\"stationName\":\""); j += jsonEscape(audioStationDisplayName(audioCurrentStation())); j += F("\",");
    j += F("\"icy\":\"");      j += jsonEscape(audioCurStation());  j += F("\",");
    j += F("\"song\":\"");     j += jsonEscape(audioSongPlaying()); j += F("\",");
    j += F("\"bitrate\":");    j += audioBitrate();           j += F(",");
    j += F("\"volume\":");     j += audioVolume();            j += F(",");
    j += F("\"volumeRaw\":");  j += audioVolumeRaw();         j += F(",");
    j += F("\"eqBass\":");     j += audioEqBass();            j += F(",");
    j += F("\"eqMid\":");      j += audioEqMid();             j += F(",");
    j += F("\"eqTreble\":");   j += audioEqTreble();          j += F(",");
    j += F("\"running\":");    j += audioIsRunning() ? "true" : "false";
    j += F("}");
    return j;
}

// ---- Routes ---------------------------------------------------------------
void handleIndex() {
    String p = pageHead("ON8CIT WebRadio");
    int cur = audioCurrentStation();
    int n   = stationsCount();

    p += F("<div class=card><h2>Now playing</h2><div class=kv>");
    p += F("<div>Station</div><div>");
    p += htmlEscape(audioStationDisplayName(cur));
    p += F(" &mdash; slot "); p += (cur + 1); p += F(" / "); p += n;
    p += F("</div>");
    p += F("<div>Song</div><div>");
    {
        const char *sp = audioSongPlaying();
        p += (sp && *sp) ? htmlEscape(sp) : String("&mdash;");
    }
    p += F("</div>");
    p += F("<div>Bitrate</div><div>"); p += audioBitrate(); p += F(" kbps</div>");
    p += F("<div>Volume</div><div>");  p += audioVolume();  p += F(" / 5</div>");
    p += F("<div>WiFi</div><div>");    p += htmlEscape(WiFi.SSID());
    p += F(" ("); p += netRssi(); p += F(" dBm)</div>");
    p += F("</div></div>");

    p += F("<div class=card><h2>Controls</h2><div class=row>");
    p += F("<form method=POST action=/api/prev><button>Prev</button></form>");
    p += F("<form method=POST action=/api/next><button>Next</button></form>");
    p += F("<form method=POST action=/api/reboot onsubmit=\"return confirm('Reboot the radio?')\">"
           "<button class=warn>Reboot</button></form>");
    p += F("</div>");

    // Fine volume slider (0..21). Uses POST /api/volume?raw=N.
    p += F("<form method=POST action=/api/volume style='margin-top:.8em'>"
           "<label>Volume (fine, 0&ndash;21): <output name=vo id=vo>");
    p += audioVolumeRaw();
    p += F("</output></label>"
           "<input type=range name=raw min=0 max=21 value=");
    p += audioVolumeRaw();
    p += F(" oninput=\"vo.value=this.value\">"
           "<button type=submit>Apply</button></form>");

    // 3-band EQ. Values -40..+6 dB.
    p += F("<form method=POST action=/api/eq style='margin-top:.8em'>"
           "<label>Bass <output id=eb>"); p += audioEqBass();
    p += F("</output> dB <input type=range name=b min=-40 max=6 value=");
    p += audioEqBass();
    p += F(" oninput=\"eb.value=this.value\"></label>");
    p += F("<label>Mid <output id=em>"); p += audioEqMid();
    p += F("</output> dB <input type=range name=m min=-40 max=6 value=");
    p += audioEqMid();
    p += F(" oninput=\"em.value=this.value\"></label>");
    p += F("<label>Treble <output id=et>"); p += audioEqTreble();
    p += F("</output> dB <input type=range name=t min=-40 max=6 value=");
    p += audioEqTreble();
    p += F(" oninput=\"et.value=this.value\"></label>");
    p += F("<button type=submit>Apply EQ</button></form>");
    p += F("</div>");

    p += F("<div class=card><h2>Stations</h2><div class=grid>");
    for (int i = 0; i < n; i++) {
        p += F("<div class=stationRow>");
        p += F("<form method=POST action=/api/station class=stationPick>");
        p += F("<input type=hidden name=n value="); p += i; p += F(">");
        p += F("<button class='station");
        if (i == cur) p += F(" cur");
        p += F("' type=submit><strong>"); p += (i + 1); p += F(".</strong> ");
        p += htmlEscape(audioStationDisplayName(i));
        p += F("<small>"); p += htmlEscape(stationsUrl(i)); p += F("</small>");
        p += F("</button></form>");
        // Info / edit button opens the per-station modal.
        p += F("<button type=button class=infoBtn title='Edit name + URL' "
               "onclick=\"openEdit(");
        p += i; p += F(",");
        p += "'"; p += htmlEscape(stationsName(i)); p += "',";
        p += "'"; p += htmlEscape(stationsUrl(i)); p += "',";
        p += "'"; p += htmlEscape(stationsOverrideName(i)); p += "',";
        p += "'"; p += htmlEscape(stationsOverrideUrl(i)); p += "')\">&#9432;</button>";
        p += F("</div>");
    }
    p += F("</div></div>");

    // Modal + script for /api/station-edit.
    p += F(
      "<div id=modalBg onclick=\"closeEdit(event)\"></div>"
      "<div id=modal>"
      "<h2>Edit station <span id=mi></span></h2>"
      "<p style='color:#666;margin-top:-.4em;font-size:.9em'>"
      "Leave a field blank to use the default. Reset clears overrides.</p>"
      "<form method=POST action=/api/station-edit>"
      "<input type=hidden name=n id=mn>"
      "<label>Friendly name"
      "<input name=name id=mname maxlength=60 placeholder='(default)'></label>"
      "<label>URL"
      "<input name=url id=murl maxlength=200 placeholder='(default)'></label>"
      "<div class=row style='margin-top:.6em'>"
      "<button type=submit>Save</button>"
      "<button type=submit name=reset value=1 class=warn>Reset</button>"
      "<button type=button onclick=\"closeEdit()\">Cancel</button>"
      "</div></form></div>"
      "<script>"
      "function openEdit(n,defName,defUrl,ovName,ovUrl){"
      " mi.textContent=n+1;"
      " mn.value=n;"
      " mname.value=ovName||'';"
      " murl.value=ovUrl||'';"
      " mname.placeholder=defName||'(default)';"
      " murl.placeholder=defUrl||'(default)';"
      " modal.classList.add('show');"
      " modalBg.classList.add('show');"
      "}"
      "function closeEdit(e){"
      " if(e&&e.target&&e.target.id!=='modalBg')return;"
      " modal.classList.remove('show');"
      " modalBg.classList.remove('show');"
      "}"
      "</script>");

    p += F("<div class=card><h2>API</h2><div class=kv>"
           "<div>GET /api/state</div><div>Full state as JSON</div>"
           "<div>POST /api/station</div><div>Body: n=&lt;0..N-1&gt;</div>"
           "<div>POST /api/station-edit</div><div>Body: n=&lt;slot&gt; name=... url=... (or reset=1)</div>"
           "<div>POST /api/next</div><div></div>"
           "<div>POST /api/prev</div><div></div>"
           "<div>POST /api/volume</div><div>Body: raw=&lt;0..21&gt;  or  v=&lt;1..5&gt;  or  d=&pm;1</div>"
           "<div>POST /api/eq</div><div>Body: b=&lt;-40..6&gt; m=&lt;-40..6&gt; t=&lt;-40..6&gt;</div>"
           "<div>POST /api/reboot</div><div></div>"
           "</div></div>");

    p += pageFoot();
    s_http.send(200, "text/html", p);
}

void handleState() {
    s_http.send(200, "application/json", stateJson());
}

void handleStation() {
    if (!s_http.hasArg("n")) { s_http.send(400, "text/plain", "missing n"); return; }
    int n = s_http.arg("n").toInt();
    if (n < 0 || n >= stationsCount()) { s_http.send(400, "text/plain", "out of range"); return; }
    audioSelectStation(n);
    s_http.sendHeader("Location", "/");
    s_http.send(302);
}

void handleStationEdit() {
    if (!s_http.hasArg("n")) { s_http.send(400, "text/plain", "missing n"); return; }
    int n = s_http.arg("n").toInt();
    if (n < 0 || n >= stationsCount()) { s_http.send(400, "text/plain", "out of range"); return; }
    if (s_http.hasArg("reset")) {
        stationsResetSlot(n);
    } else {
        String name = s_http.arg("name"); name.trim();
        String url  = s_http.arg("url");  url.trim();
        if (!stationsSetSlot(n, name, url)) {
            s_http.send(500, "text/plain", "save failed (NVS full)");
            return;
        }
    }
    s_http.sendHeader("Location", "/");
    s_http.send(302);
}

void handleNext()  { audioNextStation(); s_http.sendHeader("Location", "/"); s_http.send(302); }
void handlePrev()  { audioPrevStation(); s_http.sendHeader("Location", "/"); s_http.send(302); }

void handleVolume() {
    if (s_http.hasArg("raw")) {
        audioSetVolumeRaw(s_http.arg("raw").toInt());
    } else if (s_http.hasArg("v")) {
        audioSetVolume(s_http.arg("v").toInt());
    } else if (s_http.hasArg("d")) {
        int delta = s_http.arg("d").toInt();
        audioSetVolume(audioVolume() + delta);
    }
    s_http.sendHeader("Location", "/");
    s_http.send(302);
}

void handleEq() {
    int8_t b = audioEqBass();
    int8_t m = audioEqMid();
    int8_t t = audioEqTreble();
    if (s_http.hasArg("b")) b = (int8_t)s_http.arg("b").toInt();
    if (s_http.hasArg("m")) m = (int8_t)s_http.arg("m").toInt();
    if (s_http.hasArg("t")) t = (int8_t)s_http.arg("t").toInt();
    audioSetEq(b, m, t);
    s_http.sendHeader("Location", "/");
    s_http.send(302);
}

bool s_rebootPending = false;
uint32_t s_rebootAtMs = 0;

void handleReboot() {
    String p = pageHead("Rebooting");
    p += F("<div class=card><h2>Rebooting...</h2><p>The radio is restarting. "
           "This page will be unreachable for a few seconds.</p></div>");
    p += pageFoot();
    s_http.send(200, "text/html", p);
    s_rebootPending = true;
    s_rebootAtMs    = millis() + 1500;
}

void handleNotFound() { s_http.send(404, "text/plain", "not found"); }
} // namespace

void webBegin() {
    if (s_running) return;
    if (WiFi.status() != WL_CONNECTED) return;

    // mDNS: announce on the LAN so the radio is reachable at
    // http://on8cit-radio.local without typing the IP.
    if (MDNS.begin(netHostname())) {
        MDNS.addService("http", "tcp", 80);
        MDNS.addServiceTxt("http", "tcp", "version", FIRMWARE_VERSION);
        MDNS.addServiceTxt("http", "tcp", "name",    FIRMWARE_NAME);
        s_mdnsUp = true;
        Serial.printf("web: mDNS up at http://%s.local\r\n", netHostname());
    } else {
        Serial.println("web: mDNS begin FAILED (still reachable via IP)");
    }

    s_http.on("/",             HTTP_GET,  handleIndex);
    s_http.on("/api/state",    HTTP_GET,  handleState);
    s_http.on("/api/station",       HTTP_POST, handleStation);
    s_http.on("/api/station-edit",  HTTP_POST, handleStationEdit);
    s_http.on("/api/next",     HTTP_POST, handleNext);
    s_http.on("/api/prev",     HTTP_POST, handlePrev);
    s_http.on("/api/volume",   HTTP_POST, handleVolume);
    s_http.on("/api/eq",       HTTP_POST, handleEq);
    s_http.on("/api/reboot",   HTTP_POST, handleReboot);
    s_http.onNotFound(handleNotFound);
    s_http.begin();
    s_running = true;
    Serial.printf("web: http server up at http://%s/\r\n", WiFi.localIP().toString().c_str());
}

void webPoll() {
    if (!s_running) return;
    // Record activity whenever a client is connected -- approximates
    // "someone is looking at the web UI", enough to wake the panel.
    if (s_http.client()) displayNoteActivity();
    s_http.handleClient();

    // mDNS heartbeat. WiFi.status() check avoids touching MDNS while the
    // STA interface is disconnected (which would leave the service record
    // orphaned and confuse the next begin()).
    if (WiFi.status() == WL_CONNECTED &&
        (millis() - s_mdnsLastCheckMs) > kMdnsHeartbeatMs) {
        s_mdnsLastCheckMs = millis();
        MDNS.end();
        if (MDNS.begin(netHostname())) {
            MDNS.addService("http", "tcp", 80);
            MDNS.addServiceTxt("http", "tcp", "version", FIRMWARE_VERSION);
            MDNS.addServiceTxt("http", "tcp", "name",    FIRMWARE_NAME);
            s_mdnsUp = true;
        }
    }

    if (s_rebootPending && (int32_t)(millis() - s_rebootAtMs) >= 0) {
        s_http.stop();
        if (s_mdnsUp) MDNS.end();
        delay(100);
        ESP.restart();
    }
}

void webStop() {
    if (!s_running) return;
    s_http.stop();
    if (s_mdnsUp) { MDNS.end(); s_mdnsUp = false; }
    s_running = false;
}

bool webRunning() { return s_running; }
