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

// ---- blocking line read -------------------------------------------------

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

static const char *encStr(uint8_t e) {
    switch (e) {
        case WIFI_AUTH_OPEN:            return "OPEN";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/2";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/3";
        default:                        return "?";
    }
}

// ---- commands ------------------------------------------------------------

static void cmdHelp() {
    outln();
    outln(F("=== Waveshare Internet Radio ==="));
    outln(F("  help, ?                Show this help"));
    outln(F("  status, stat, s        Show radio status"));
    outln(F("  stations, list         List preset stations"));
    outln(F("  station <n>, sel <n>   Select station N (1-based)"));
    outln(F("  next / prev            Cycle stations"));
    outln(F("  volume [n], vol [n]    Show or set volume (1..5)"));
    outln(F("  vol+ / vol- / + / -    Step volume"));
    outln();
    outln(F("  wifi scan              Scan visible networks"));
    outln(F("  wifi list              List saved networks (ordered)"));
    outln(F("  wifi add [ssid]        Add a network (interactive: scan + pick + pass)"));
    outln(F("  wifi                   Alias for 'wifi add'"));
    outln(F("  wifi remove <n>        Remove saved network N"));
    outln(F("  wifi move <from> <to>  Reorder saved networks"));
    outln(F("  wifi clear             Erase ALL saved networks"));
    outln(F("  reconnect              Try saved networks again"));
    outln();
    outln(F("  log on | off           Stream audio library events to serial"));
    outln(F("  reboot, r              Restart the device"));
    outln(F("  sleep                  Deep sleep (wake via left button)"));
    outln();
}

static void cmdStatus() {
    outln();
    Serial.print(F("WiFi    : ")); outln(netConnected() ? "connected" : "disconnected");
    Serial.print(F("SSID    : "));
    outln(netConnected() ? WiFi.SSID() : String("<not connected>"));
    if (netConnected()) {
        Serial.print(F("IP      : ")); outln(netLocalIp());
        Serial.print(F("RSSI    : ")); Serial.print(netRssi()); outln(F(" dBm"));
    }
    Serial.print(F("Saved   : "));
    Serial.print(wifiNetworkCount()); outln(F(" network(s) (see 'wifi list')"));
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

// ---- WiFi commands -------------------------------------------------------

static void cmdWifiScan() {
    outln(F("Scanning..."));
    int n = netScanNow();
    if (n == 0) { outln(F("No networks found.")); return; }
    outln();
    for (int i = 0; i < n; i++) {
        const ScanResult *r = netScanResult(i);
        if (i + 1 < 10) Serial.write(' ');
        Serial.print(i + 1); Serial.print(F("  "));
        Serial.print(r->rssi); Serial.print(F(" dBm  "));
        Serial.print(encStr(r->encryption)); Serial.print(F("  "));
        outln(r->ssid);
    }
    outln();
}

static void cmdWifiList() {
    outln();
    int n = wifiNetworkCount();
    if (n == 0) { outln(F("No saved networks.")); return; }
    for (int i = 0; i < n; i++) {
        Serial.print(F("  ")); Serial.print(i + 1); Serial.print(F(". "));
        Serial.print(wifiNetworkSsid(i));
        size_t plen = strlen(wifiNetworkPass(i));
        if (plen > 0) { Serial.print(F("  (pass: ")); Serial.print(plen); outln(F(" chars)")); }
        else          { outln(F("  (open)")); }
    }
    outln();
}

static void cmdWifiAdd(const String &presetSsid) {
    // If no SSID passed, offer a scan-and-pick. Otherwise go straight to
    // password entry for the supplied SSID.
    String ssid = presetSsid; ssid.trim();

    if (ssid.length() == 0) {
        outln(F("Scan networks? [Y/n]"));
        String yn = readLineBlocking("> ", false); yn.trim(); yn.toLowerCase();
        if (yn.length() == 0 || yn.startsWith("y")) {
            int n = netScanNow();
            if (n == 0) { outln(F("No networks found. Enter SSID manually.")); }
            else {
                outln();
                for (int i = 0; i < n; i++) {
                    const ScanResult *r = netScanResult(i);
                    if (i + 1 < 10) Serial.write(' ');
                    Serial.print(i + 1); Serial.print(F("  "));
                    Serial.print(r->rssi); Serial.print(F(" dBm  "));
                    Serial.print(encStr(r->encryption)); Serial.print(F("  "));
                    outln(r->ssid);
                }
                outln();
                String sel = readLineBlocking("Pick # (or blank to type SSID): ", false);
                sel.trim();
                if (sel.length() > 0) {
                    int idx = sel.toInt();
                    if (idx >= 1 && idx <= n) ssid = netScanResult(idx - 1)->ssid;
                }
            }
        }
        if (ssid.length() == 0) {
            ssid = readLineBlocking("SSID: ", false); ssid.trim();
        }
    }
    if (ssid.length() == 0) { outln(F("Aborted (empty SSID).")); return; }

    String pass = readLineBlocking("Password: ", true);

    if (!wifiAddNetwork(ssid, pass)) {
        if (wifiNetworkCount() >= kWifiMaxNetworks)
            outln(F("Storage full. Remove one first."));
        else
            outln(F("Failed to save."));
        return;
    }
    Serial.print(F("Saved '")); Serial.print(ssid);
    outln(F("'. Use 'reconnect' or 'reboot' to apply."));
}

static void cmdWifiRemove(const String &arg) {
    int n = arg.toInt();
    if (n < 1 || n > wifiNetworkCount()) {
        Serial.print(F("Index must be 1..")); Serial.println(wifiNetworkCount());
        return;
    }
    String name = wifiNetworkSsid(n - 1);
    wifiRemoveNetwork(n - 1);
    Serial.print(F("Removed '")); Serial.print(name); outln(F("'."));
}

static void cmdWifiMove(const String &arg) {
    int sp = arg.indexOf(' ');
    if (sp < 0) { outln(F("Usage: wifi move <from> <to>")); return; }
    int from = arg.substring(0, sp).toInt();
    int to   = arg.substring(sp + 1).toInt();
    int n = wifiNetworkCount();
    if (from < 1 || from > n || to < 1 || to > n) {
        Serial.print(F("Indices must be 1..")); Serial.println(n); return;
    }
    wifiMoveNetwork(from - 1, to - 1);
    outln(F("Moved."));
}

static void cmdWifiClear() {
    outln(F("This erases ALL saved networks. Type 'yes' to confirm:"));
    String y = readLineBlocking("> ", false); y.trim();
    if (y.equalsIgnoreCase("yes")) {
        wifiClearAllNetworks();
        outln(F("All saved networks erased."));
    } else {
        outln(F("Aborted."));
    }
}

static void cmdReconnect() {
    if (wifiNetworkCount() == 0) { outln(F("No saved networks. Run 'wifi add' first.")); return; }
    outln(F("Reconnecting..."));
    netReconnect();
    outln(netConnected() ? "OK." : "No saved network connected.");
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
    else if (eqi(cmd, "wifi") && arg.length() == 0)                          cmdWifiAdd("");
    else if (eqi(cmd, "wifi") && eqi(arg, "scan"))                           cmdWifiScan();
    else if (eqi(cmd, "wifi") && eqi(arg, "list"))                           cmdWifiList();
    else if (eqi(cmd, "wifi") && eqi(arg, "clear"))                          cmdWifiClear();
    else if (eqi(cmd, "wifi") && arg.startsWith("add")) {
        String rest = arg.substring(3); rest.trim();
        cmdWifiAdd(rest);
    }
    else if (eqi(cmd, "wifi") && arg.startsWith("remove")) {
        String rest = arg.substring(6); rest.trim();
        cmdWifiRemove(rest);
    }
    else if (eqi(cmd, "wifi") && arg.startsWith("move")) {
        String rest = arg.substring(4); rest.trim();
        cmdWifiMove(rest);
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
    if (wifiHasNetworks()) return;
    outln();
    outln(F("No WiFi networks saved yet."));
    outln(F("Run 'wifi add' to configure one (or use the AP portal when offered)."));
    // Interactive add, loop until at least one network is saved.
    while (!wifiHasNetworks()) {
        cmdWifiAdd("");
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
