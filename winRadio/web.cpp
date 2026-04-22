#include "web.h"
#include "config.h"
#include "radio_audio.h"
#include "stations.h"
#include "storage.h"
#include "net.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>

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
    p.reserve(1200);
    p += F("<!doctype html><html><head><meta charset=utf-8>"
           "<meta name=viewport content='width=device-width,initial-scale=1'>"
           "<title>");
    p += title;
    p += F("</title><style>"
           "body{font-family:-apple-system,sans-serif;max-width:640px;margin:0 auto;padding:0 1em;color:#222;background:#f7f7f9}"
           "header{background:#111;color:#fff;margin:0 -1em 1em;padding:.9em 1em;display:flex;align-items:center;gap:.6em}"
           "header .dot{width:14px;height:14px;border-radius:50%;background:#fb0;box-shadow:0 0 0 4px #f803,0 0 0 10px #f801}"
           "header h1{margin:0;font-size:1.1em;font-weight:600;letter-spacing:.04em}"
           "header h1 span{color:#5cf}"
           ".card{background:#fff;border:1px solid #ddd;border-radius:6px;padding:.8em 1em;margin:.6em 0}"
           ".card h2{margin:0 0 .4em;font-size:1em;color:#555}"
           ".kv{display:grid;grid-template-columns:8em 1fr;gap:.2em 1em;font-family:monospace;font-size:.9em}"
           ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(180px,1fr));gap:.4em}"
           ".station{padding:.55em .7em;border:1px solid #ccc;border-radius:5px;background:#fff;cursor:pointer;text-decoration:none;color:#222;display:block}"
           ".station.cur{border-color:#18c;background:#e6f2fb}"
           ".station small{display:block;color:#888;margin-top:.2em;word-break:break-all}"
           "button,.btn{background:#18c;color:#fff;border:0;padding:.55em 1em;border-radius:4px;cursor:pointer;font-size:1em}"
           "button.warn{background:#b33}"
           ".row{display:flex;gap:.4em;align-items:center;flex-wrap:wrap}"
           "footer{color:#888;text-align:center;margin:1em 0;font-size:.85em}"
           "</style></head><body>"
           "<header><div class=dot></div><h1>ON8CIT <span>WebRadio</span></h1></header>");
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
    p += F("<form method=POST action=/api/volume><input type=hidden name=d value=-1>"
           "<button>Vol &minus;</button></form>");
    p += F("<form method=POST action=/api/volume><input type=hidden name=d value=1>"
           "<button>Vol &plus;</button></form>");
    p += F("<form method=POST action=/api/prev><button>Prev</button></form>");
    p += F("<form method=POST action=/api/next><button>Next</button></form>");
    p += F("<form method=POST action=/api/reboot onsubmit=\"return confirm('Reboot the radio?')\">"
           "<button class=warn>Reboot</button></form>");
    p += F("</div></div>");

    p += F("<div class=card><h2>Stations</h2><div class=grid>");
    for (int i = 0; i < n; i++) {
        p += F("<form method=POST action=/api/station>");
        p += F("<input type=hidden name=n value="); p += i; p += F(">");
        p += F("<button class='station");
        if (i == cur) p += F(" cur");
        p += F("' type=submit><strong>"); p += (i + 1); p += F(".</strong> ");
        p += htmlEscape(audioStationDisplayName(i));
        p += F("<small>"); p += htmlEscape(stationsUrl(i)); p += F("</small>");
        p += F("</button></form>");
    }
    p += F("</div></div>");

    p += F("<div class=card><h2>API</h2><div class=kv>"
           "<div>GET /api/state</div><div>Full state as JSON</div>"
           "<div>POST /api/station</div><div>Body: n=&lt;0..N-1&gt;</div>"
           "<div>POST /api/next</div><div></div>"
           "<div>POST /api/prev</div><div></div>"
           "<div>POST /api/volume</div><div>Body: v=&lt;1..5&gt; or d=&pm;1</div>"
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

void handleNext()  { audioNextStation(); s_http.sendHeader("Location", "/"); s_http.send(302); }
void handlePrev()  { audioPrevStation(); s_http.sendHeader("Location", "/"); s_http.send(302); }

void handleVolume() {
    if (s_http.hasArg("v")) {
        audioSetVolume(s_http.arg("v").toInt());
    } else if (s_http.hasArg("d")) {
        int delta = s_http.arg("d").toInt();
        int v = audioVolume() + delta;
        audioSetVolume(v);
    }
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
    s_http.on("/api/station",  HTTP_POST, handleStation);
    s_http.on("/api/next",     HTTP_POST, handleNext);
    s_http.on("/api/prev",     HTTP_POST, handlePrev);
    s_http.on("/api/volume",   HTTP_POST, handleVolume);
    s_http.on("/api/reboot",   HTTP_POST, handleReboot);
    s_http.onNotFound(handleNotFound);
    s_http.begin();
    s_running = true;
    Serial.printf("web: http server up at http://%s/\r\n", WiFi.localIP().toString().c_str());
}

void webPoll() {
    if (!s_running) return;
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
