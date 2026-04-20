// Serial CLI + NVS-backed WiFi credential storage.
// Runs on Serial at 9600 8N1 (configured in winRadio.ino).
//
// Line-ending policy: accept CR, LF, or CRLF as Enter (PuTTY default sends
// CR only). All output is emitted as CRLF so PuTTY and other terminals
// render lines correctly without needing "Implicit LF in every CR".

#include "cli.h"
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>

static const char *PROMPT = "radio> ";

static Preferences g_prefs;
static String      g_ssid = "";
static String      g_pass = "";
static String      g_buf  = "";
static bool        g_lastWasCr = false;

// --------------------------------------------------------------------------
// Output helpers: always CRLF.
// --------------------------------------------------------------------------

static void crlf() { Serial.write('\r'); Serial.write('\n'); }

static void outln() { crlf(); }
static void outln(const char *s) { Serial.print(s); crlf(); }
static void outln(const String &s) { Serial.print(s); crlf(); }
static void outln(const __FlashStringHelper *s) { Serial.print(s); crlf(); }

static void prompt() { Serial.print(PROMPT); }

// --------------------------------------------------------------------------
// Credential storage.
// --------------------------------------------------------------------------

bool loadWifiCreds() {
    g_prefs.begin("wifi", true);
    g_ssid = g_prefs.getString("ssid", "");
    g_pass = g_prefs.getString("pass", "");
    g_prefs.end();
    return g_ssid.length() > 0;
}

static void saveWifiCreds(const String &s, const String &p) {
    g_prefs.begin("wifi", false);
    g_prefs.putString("ssid", s);
    g_prefs.putString("pass", p);
    g_prefs.end();
    g_ssid = s;
    g_pass = p;
}

static void clearWifiCreds() {
    g_prefs.begin("wifi", false);
    g_prefs.remove("ssid");
    g_prefs.remove("pass");
    g_prefs.end();
    g_ssid = "";
    g_pass = "";
}

bool        hasWifiCreds()    { return g_ssid.length() > 0; }
const char *getWifiSsid()     { return g_ssid.c_str(); }
const char *getWifiPassword() { return g_pass.c_str(); }

// --------------------------------------------------------------------------
// Blocking line read (used only during first-run WiFi setup).
// --------------------------------------------------------------------------

static String readLineBlocking(const char *label, bool mask) {
    Serial.print(label);
    String line = "";
    bool lastCr = false;
    while (true) {
        if (Serial.available()) {
            char c = Serial.read();
            if (c == '\n' && lastCr) { lastCr = false; continue; }
            lastCr = (c == '\r');
            if (c == '\r' || c == '\n') { crlf(); return line; }
            if (c == 8 || c == 127) {
                if (line.length()) {
                    line.remove(line.length() - 1);
                    Serial.print("\b \b");
                }
                continue;
            }
            if (c < 0x20 || c > 0x7E) continue;
            line += c;
            Serial.write(mask ? '*' : c);
        }
        delay(1);
    }
}

// --------------------------------------------------------------------------
// Commands.
// --------------------------------------------------------------------------

static void cmdHelp() {
    outln();
    outln(F("=== Waveshare Internet Radio ==="));
    outln(F("  help, ?              Show this help"));
    outln(F("  status, s            Show radio status"));
    outln(F("  stations, list       List preset stations"));
    outln(F("  station <n>, sel <n> Select station N (1-based)"));
    outln(F("  next                 Next station"));
    outln(F("  prev                 Previous station"));
    outln(F("  volume, vol          Show current volume"));
    outln(F("  volume <n>           Set volume (1..5)"));
    outln(F("  vol+, +              Volume up"));
    outln(F("  vol-, -              Volume down"));
    outln(F("  wifi                 Enter new SSID + password"));
    outln(F("  wifi show            Show stored SSID"));
    outln(F("  wifi clear           Erase stored credentials"));
    outln(F("  reconnect            Reconnect to WiFi"));
    outln(F("  reboot, r            Restart the device"));
    outln(F("  sleep                Deep sleep (wake via left button)"));
    outln();
}

static void cmdStatus() {
    outln();
    Serial.print(F("WiFi    : "));
    outln(WiFi.status() == WL_CONNECTED ? "connected" : "disconnected");
    Serial.print(F("SSID    : "));
    outln(g_ssid.length() ? g_ssid : String("<unset>"));
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print(F("IP      : "));
        outln(WiFi.localIP().toString());
        Serial.print(F("RSSI    : "));
        Serial.print(WiFi.RSSI());
        outln(F(" dBm"));
    }
    Serial.print(F("Station : "));
    Serial.print(radioCurrentStation() + 1);
    Serial.print(F(" / "));
    Serial.print(radioStationCount());
    Serial.print(F("  "));
    outln(radioStationUrl(radioCurrentStation()));
    Serial.print(F("Song    : "));
    {
        const char *sp = radioSongPlaying();
        outln((sp && *sp) ? sp : "<none>");
    }
    Serial.print(F("Volume  : "));
    Serial.println(radioVolume());
    Serial.print(F("Bitrate : "));
    Serial.print(radioBitrate());
    outln(F(" kbps"));
    Serial.print(F("Battery : "));
    Serial.print(radioBattery(), 2);
    outln(F(" V"));
    outln();
}

static void cmdListStations() {
    outln();
    int n = radioStationCount();
    int cur = radioCurrentStation();
    for (int i = 0; i < n; i++) {
        Serial.print(i == cur ? F(" * ") : F("   "));
        if (i + 1 < 10) Serial.write(' ');
        Serial.print(i + 1);
        Serial.print(F("  "));
        outln(radioStationUrl(i));
    }
    outln();
}

static void cmdSelectStation(const String &arg) {
    int n = arg.toInt();
    if (n < 1 || n > radioStationCount()) {
        Serial.print(F("Station index must be 1..")); Serial.println(radioStationCount());
        return;
    }
    radioSelectStation(n - 1);
    Serial.print(F("Switched to station "));
    Serial.print(n);
    Serial.print(F(": "));
    outln(radioStationUrl(n - 1));
}

static void cmdVolume(const String &arg) {
    if (arg.length() == 0) {
        Serial.print(F("Volume: "));
        Serial.println(radioVolume());
        return;
    }
    int v = arg.toInt();
    if (v < 1 || v > 5) {
        outln(F("Volume must be 1..5"));
        return;
    }
    radioSetVolume(v);
    Serial.print(F("Volume: "));
    Serial.println(radioVolume());
}

static void cmdVolBump(int delta) {
    int v = radioVolume() + delta;
    if (v < 1) v = 1;
    if (v > 5) v = 5;
    radioSetVolume(v);
    Serial.print(F("Volume: "));
    Serial.println(radioVolume());
}

static void cmdWifiSet() {
    String s = readLineBlocking("SSID: ", false);
    s.trim();
    if (s.length() == 0) { outln(F("Aborted (empty SSID).")); return; }
    String p = readLineBlocking("Password: ", true);
    saveWifiCreds(s, p);
    outln(F("Saved to NVS. Use 'reconnect' or 'reboot' to apply."));
}

static void cmdReconnect() {
    if (!hasWifiCreds()) { outln(F("No credentials stored. Run 'wifi' first.")); return; }
    outln(F("Reconnecting..."));
    radioReconnectWifi();
}

// --------------------------------------------------------------------------
// Dispatcher.
// --------------------------------------------------------------------------

static bool eqi(const String &a, const char *b) { return a.equalsIgnoreCase(b); }

static void dispatch(const String &raw) {
    String line = raw;
    line.trim();
    if (line.length() == 0) return;

    String cmd, arg;
    int sp = line.indexOf(' ');
    if (sp < 0) { cmd = line; }
    else        { cmd = line.substring(0, sp); arg = line.substring(sp + 1); arg.trim(); }

    if      (eqi(cmd, "help") || cmd == "?")                   cmdHelp();
    else if (eqi(cmd, "status") || eqi(cmd, "s"))              cmdStatus();
    else if (eqi(cmd, "stations") || eqi(cmd, "list"))         cmdListStations();
    else if (eqi(cmd, "station") || eqi(cmd, "sel"))           cmdSelectStation(arg);
    else if (eqi(cmd, "next"))                                 { radioNextStation(); cmdStatus(); }
    else if (eqi(cmd, "prev"))                                 { radioPrevStation(); cmdStatus(); }
    else if (eqi(cmd, "volume") || eqi(cmd, "vol") || eqi(cmd, "v")) cmdVolume(arg);
    else if (eqi(cmd, "vol+") || cmd == "+")                   cmdVolBump(+1);
    else if (eqi(cmd, "vol-") || cmd == "-")                   cmdVolBump(-1);
    else if (eqi(cmd, "wifi") && arg.length() == 0)            cmdWifiSet();
    else if (eqi(cmd, "wifi") && eqi(arg, "show")) {
        if (g_ssid.length() == 0) outln(F("No SSID stored."));
        else { Serial.print(F("SSID: ")); outln(g_ssid); }
    }
    else if (eqi(cmd, "wifi") && eqi(arg, "clear")) {
        clearWifiCreds();
        outln(F("Credentials cleared."));
    }
    else if (eqi(cmd, "reconnect"))                            cmdReconnect();
    else if (eqi(cmd, "reboot") || eqi(cmd, "r"))              { outln(F("Rebooting...")); delay(100); ESP.restart(); }
    else if (eqi(cmd, "sleep"))                                { outln(F("Sleeping...")); delay(100); radioDeepSleep(); }
    else {
        Serial.print(F("Unknown command: '"));
        Serial.print(cmd);
        outln(F("'. Type 'help' for commands."));
    }
}

// --------------------------------------------------------------------------
// Public entry points.
// --------------------------------------------------------------------------

void cliBegin() {
    crlf();
    outln(F("Waveshare Internet Radio -- serial CLI"));
    outln(F("9600 8N1  |  type 'help' for commands"));
    prompt();
}

void cliFirstRunSetup() {
    if (hasWifiCreds()) return;
    outln();
    outln(F("No WiFi credentials in NVS."));
    outln(F("Enter them via this serial console (9600 8N1)."));
    while (!hasWifiCreds()) {
        String s = readLineBlocking("SSID: ", false);
        s.trim();
        if (s.length() == 0) { outln(F("Empty SSID; try again.")); continue; }
        String p = readLineBlocking("Password: ", true);
        saveWifiCreds(s, p);
        outln(F("Saved to NVS."));
    }
}

void cliPoll() {
    while (Serial.available()) {
        char c = Serial.read();

        // Normalise line endings: accept CR, LF, or CRLF as Enter.
        if (c == '\n' && g_lastWasCr) { g_lastWasCr = false; continue; }
        g_lastWasCr = (c == '\r');

        if (c == '\r' || c == '\n') {
            crlf();
            String line = g_buf;
            g_buf = "";
            dispatch(line);
            prompt();
            continue;
        }
        if (c == 8 || c == 127) {
            if (g_buf.length()) {
                g_buf.remove(g_buf.length() - 1);
                Serial.print("\b \b");
            }
            continue;
        }
        if (c >= 0x20 && c <= 0x7E) {
            g_buf += c;
            Serial.write(c);
        }
        // Silently drop other control chars.
    }
}
