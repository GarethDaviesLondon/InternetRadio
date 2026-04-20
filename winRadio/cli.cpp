// Serial CLI. Line endings: accept CR / LF / CRLF; emit CRLF.
// All real work goes through module APIs (audio, net, storage, power).

#include "cli.h"
#include "radio_audio.h"
#include "net.h"
#include "storage.h"
#include "stations.h"
#include "power.h"

#include <Arduino.h>
#include <WiFi.h>

static const char *PROMPT = "radio> ";

static String g_buf;
static bool   g_lastWasCr = false;

// ---- output helpers (CRLF always) ---------------------------------------

static void crlf() { Serial.write('\r'); Serial.write('\n'); }
static void outln() { crlf(); }
static void outln(const char *s) { Serial.print(s); crlf(); }
static void outln(const String &s) { Serial.print(s); crlf(); }
static void outln(const __FlashStringHelper *s) { Serial.print(s); crlf(); }
static void prompt() { Serial.print(PROMPT); }

// ---- blocking line read (used only during first-run WiFi setup) ---------

static String readLineBlocking(const char *label, bool mask) {
    Serial.print(label);
    String line; bool lastCr = false;
    while (true) {
        if (Serial.available()) {
            char c = Serial.read();
            if (c == '\n' && lastCr) { lastCr = false; continue; }
            lastCr = (c == '\r');
            if (c == '\r' || c == '\n') { crlf(); return line; }
            if (c == 8 || c == 127) {
                if (line.length()) { line.remove(line.length() - 1); Serial.print("\b \b"); }
                continue;
            }
            if (c < 0x20 || c > 0x7E) continue;
            line += c;
            Serial.write(mask ? '*' : c);
        }
        delay(1);
    }
}

// ---- commands ------------------------------------------------------------

static void cmdHelp() {
    outln();
    outln(F("=== Waveshare Internet Radio ==="));
    outln(F("  help, ?              Show this help"));
    outln(F("  status, stat, s      Show radio status"));
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
    outln(F("  log on | off         Stream audio library events to serial"));
    outln(F("  reboot, r            Restart the device"));
    outln(F("  sleep                Deep sleep (wake via left button)"));
    outln();
}

static void cmdStatus() {
    outln();
    Serial.print(F("WiFi    : ")); outln(netConnected() ? "connected" : "disconnected");
    Serial.print(F("SSID    : "));
    outln(storageHasWifiCreds() ? String(storageWifiSsid()) : String("<unset>"));
    if (netConnected()) {
        Serial.print(F("IP      : ")); outln(netLocalIp());
        Serial.print(F("RSSI    : ")); Serial.print(netRssi()); outln(F(" dBm"));
    }
    int n = stationsCount(), cur = audioCurrentStation();
    Serial.print(F("Station : "));
    Serial.print(cur + 1); Serial.print(F(" / ")); Serial.print(n);
    Serial.print(F("  "));
    outln(stationsUrl(cur));
    Serial.print(F("Song    : "));
    {
        const char *sp = audioSongPlaying();
        outln((sp && *sp) ? sp : "<none>");
    }
    Serial.print(F("Volume  : ")); Serial.println(audioVolume());
    Serial.print(F("Bitrate : ")); Serial.print(audioBitrate()); outln(F(" kbps"));
    Serial.print(F("Audio   : ")); outln(audioIsRunning() ? "running" : "stopped");
    Serial.print(F("Events  : ")); Serial.print(audioInfoEventCount());
    outln(F(" audio_info callbacks"));
    Serial.print(F("Battery : ")); Serial.print(powerBatteryVolts(), 2); outln(F(" V"));
    outln();
}

static void cmdListStations() {
    outln();
    int n = stationsCount(), cur = audioCurrentStation();
    for (int i = 0; i < n; i++) {
        Serial.print(i == cur ? F(" * ") : F("   "));
        if (i + 1 < 10) Serial.write(' ');
        Serial.print(i + 1); Serial.print(F("  "));
        Serial.print(stationsName(i)); Serial.print(F("  -- "));
        outln(stationsUrl(i));
    }
    outln();
}

static void cmdSelectStation(const String &arg) {
    int n = arg.toInt();
    if (n < 1 || n > stationsCount()) {
        Serial.print(F("Station index must be 1.."));
        Serial.println(stationsCount()); return;
    }
    audioSelectStation(n - 1);
    Serial.print(F("Switched to station "));
    Serial.print(n); Serial.print(F(": "));
    outln(stationsUrl(n - 1));
}

static void cmdVolume(const String &arg) {
    if (arg.length() == 0) {
        Serial.print(F("Volume: ")); Serial.println(audioVolume()); return;
    }
    int v = arg.toInt();
    if (v < 1 || v > 5) { outln(F("Volume must be 1..5")); return; }
    audioSetVolume(v);
    Serial.print(F("Volume: ")); Serial.println(audioVolume());
}

static void cmdVolBump(int delta) {
    int v = audioVolume() + delta;
    if (v < 1) v = 1; if (v > 5) v = 5;
    audioSetVolume(v);
    Serial.print(F("Volume: ")); Serial.println(audioVolume());
}

static void cmdWifiSet() {
    String s = readLineBlocking("SSID: ", false); s.trim();
    if (s.length() == 0) { outln(F("Aborted (empty SSID).")); return; }
    String p = readLineBlocking("Password: ", true);
    storageSaveWifiCreds(s, p);
    outln(F("Saved to NVS. Use 'reconnect' or 'reboot' to apply."));
}

static void cmdReconnect() {
    if (!storageHasWifiCreds()) { outln(F("No credentials stored. Run 'wifi' first.")); return; }
    outln(F("Reconnecting..."));
    netReconnect();
}

// ---- dispatcher ----------------------------------------------------------

static bool eqi(const String &a, const char *b) { return a.equalsIgnoreCase(b); }

static void dispatch(const String &raw) {
    String line = raw; line.trim();
    if (line.length() == 0) return;

    String cmd, arg;
    int sp = line.indexOf(' ');
    if (sp < 0) { cmd = line; }
    else        { cmd = line.substring(0, sp); arg = line.substring(sp + 1); arg.trim(); }

    if      (eqi(cmd, "help") || cmd == "?")                                 cmdHelp();
    else if (eqi(cmd, "status") || eqi(cmd, "stat") || eqi(cmd, "s"))        cmdStatus();
    else if (eqi(cmd, "stations") || eqi(cmd, "list"))                       cmdListStations();
    else if (eqi(cmd, "station") || eqi(cmd, "sel"))                         cmdSelectStation(arg);
    else if (eqi(cmd, "next"))                                               { audioNextStation(); cmdStatus(); }
    else if (eqi(cmd, "prev"))                                               { audioPrevStation(); cmdStatus(); }
    else if (eqi(cmd, "volume") || eqi(cmd, "vol") || eqi(cmd, "v"))         cmdVolume(arg);
    else if (eqi(cmd, "vol+") || cmd == "+")                                 cmdVolBump(+1);
    else if (eqi(cmd, "vol-") || cmd == "-")                                 cmdVolBump(-1);
    else if (eqi(cmd, "wifi") && arg.length() == 0)                          cmdWifiSet();
    else if (eqi(cmd, "wifi") && eqi(arg, "show")) {
        if (!storageHasWifiCreds()) outln(F("No SSID stored."));
        else { Serial.print(F("SSID: ")); outln(storageWifiSsid()); }
    }
    else if (eqi(cmd, "wifi") && eqi(arg, "clear")) {
        storageClearWifiCreds();
        outln(F("Credentials cleared."));
    }
    else if (eqi(cmd, "reconnect"))                                          cmdReconnect();
    else if (eqi(cmd, "log")) {
        if (arg.length() == 0) { Serial.print(F("log: ")); outln(audioLogEnabled() ? "on" : "off"); }
        else if (eqi(arg, "on"))  { audioSetLogEnabled(true);  outln(F("log: on"));  }
        else if (eqi(arg, "off")) { audioSetLogEnabled(false); outln(F("log: off")); }
        else                        outln(F("Usage: log [on|off]"));
    }
    else if (eqi(cmd, "reboot") || eqi(cmd, "r")) { outln(F("Rebooting...")); delay(100); ESP.restart(); }
    else if (eqi(cmd, "sleep"))                   { outln(F("Sleeping..."));  delay(100); powerDeepSleep(); }
    else {
        Serial.print(F("Unknown command: '")); Serial.print(cmd);
        outln(F("'. Type 'help' for commands."));
    }
}

// ---- public entry points -------------------------------------------------

void cliBegin() {
    crlf();
    outln(F("Waveshare Internet Radio -- serial CLI"));
    outln(F("9600 8N1  |  type 'help' for commands"));
    prompt();
}

void cliFirstRunSetup() {
    if (storageHasWifiCreds()) return;
    outln();
    outln(F("No WiFi credentials in NVS."));
    outln(F("Enter them via this serial console."));
    while (!storageHasWifiCreds()) {
        String s = readLineBlocking("SSID: ", false); s.trim();
        if (s.length() == 0) { outln(F("Empty SSID; try again.")); continue; }
        String p = readLineBlocking("Password: ", true);
        storageSaveWifiCreds(s, p);
        outln(F("Saved to NVS."));
    }
}

void cliPoll() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' && g_lastWasCr) { g_lastWasCr = false; continue; }
        g_lastWasCr = (c == '\r');
        if (c == '\r' || c == '\n') {
            crlf();
            String line = g_buf; g_buf = "";
            dispatch(line);
            prompt();
            continue;
        }
        if (c == 8 || c == 127) {
            if (g_buf.length()) { g_buf.remove(g_buf.length() - 1); Serial.print("\b \b"); }
            continue;
        }
        if (c >= 0x20 && c <= 0x7E) { g_buf += c; Serial.write(c); }
    }
}
