// Serial CLI. Line endings: accept CR / LF / CRLF; emit CRLF.
// All real work goes through module APIs (audio, net, storage, power).

#include "cli.h"
#include "radio_audio.h"
#include "net.h"
#include "storage.h"
#include "stations.h"
#include "power.h"
#include "provision.h"
#include "display.h"
#include "log.h"
#include "clock.h"

#include <Arduino.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <FS.h>

static const char *PROMPT = "radio> ";

static String g_buf;
static bool   g_lastWasCr = false;
static bool   g_cancelSetup = false;
static volatile bool g_ctrlC = false;

void cliCancelSetup()     { g_cancelSetup = true; }
bool cliCheckInterrupt()  { return g_ctrlC; }
void cliClearInterrupt()  { g_ctrlC = false; }

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

// ---- parse helpers (used from commands + dispatcher) --------------------

static bool eqi(const String &a, const char *b) { return a.equalsIgnoreCase(b); }

// Split an arg string "subcmd rest of line" into first token (used as a
// subcommand, matched case-insensitively by the caller) and the untouched
// remainder. The remainder preserves case -- SSIDs, URLs, paths, etc. are
// data and must not be folded.
static void splitArg(const String &arg, String &first, String &rest) {
    int sp = arg.indexOf(' ');
    if (sp < 0) { first = arg; rest = ""; }
    else { first = arg.substring(0, sp); rest = arg.substring(sp + 1); rest.trim(); }
}

// ---- commands ------------------------------------------------------------

static void cmdHelp() {
    outln();
    outln(F("=== Waveshare Internet Radio ==="));
    outln(F("  help, ?                Show this help"));
    outln(F("  status, stat, s        Show radio status"));
    outln(F("  stations, list         List preset stations"));
    outln(F("  station <n>, sel <n>   Select station N (1-based)"));
    outln(F("  station edit <n>       Edit friendly name + URL for slot N"));
    outln(F("  station reset <n>      Revert slot N to the default name + URL"));
    outln(F("  next / prev            Cycle stations"));
    outln(F("  volume [n], vol [n]    Show or set volume (1..5)"));
    outln(F("  vol+ / vol- / + / -    Step volume"));
    outln();
    outln(F("  wifi scan              Scan visible networks (shows channel)"));
    outln(F("  wifi scan-hard         3-pass scan -- catches slow-beacon hotspots"));
    outln(F("  wifi list              List saved networks (ordered)"));
    outln(F("  wifi add [ssid]        Add a network (interactive: scan + pick + pass)"));
    outln(F("  wifi connect <ssid> [pass]"));
    outln(F("                         Ad-hoc connect; offers to save on success"));
    outln(F("  wifi                   Alias for 'wifi add'"));
    outln(F("  wifi remove <n>        Remove saved network N"));
    outln(F("  wifi move <from> <to>  Reorder saved networks"));
    outln(F("  wifi clear             Erase ALL saved networks"));
    outln(F("  wifi diag              Dump WiFi driver state + last reason code"));
    outln(F("  cancel                 Exit setup-mode and resume boot flow"));
    outln(F("  reconnect              Try saved networks again"));
    outln();
    outln(F("  time, time show        Show synced date/time + timezone"));
    outln(F("  time set <posix-tz>    Set POSIX TZ (e.g. GMT0BST,M3.5.0/1,M10.5.0)"));
    outln();
    outln(F("  sd status              Show SD card state"));
    outln(F("  sd ls <path>           List a directory"));
    outln(F("  sd reload              Re-read stations.csv + theme.ini"));
    outln();
    outln(F("  log                    Show all log flags"));
    outln(F("  log on | off           Stream audio library events to serial"));
    outln(F("  log verbose on | off   Enable [DEBUG] lines in the log stream"));
    outln(F("  log sd on | off        Mirror log to /log.txt on SD (rotated)"));
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

static void cmdStationEdit(const String &arg) {
    int n = arg.toInt();
    if (n < 1 || n > stationsCount()) {
        Serial.print(F("Usage: station edit <1..")); Serial.print(stationsCount()); outln(F(">"));
        return;
    }
    int idx = n - 1;
    outln();
    Serial.print(F("Editing slot ")); Serial.print(n);
    Serial.print(F(": currently '")); Serial.print(stationsName(idx));
    Serial.print(F("' @ ")); outln(stationsUrl(idx));
    outln(F("Leave a field blank to keep the current value. Type 'RESET' to clear overrides."));
    String newName = readLineBlocking("Friendly name: ", false); newName.trim();
    if (newName.equalsIgnoreCase("RESET")) {
        stationsResetSlot(idx);
        outln(F("Slot reset to defaults."));
        return;
    }
    String newUrl  = readLineBlocking("URL          : ", false); newUrl.trim();
    String keepName = (newName.length() ? newName : String(stationsOverrideName(idx)));
    String keepUrl  = (newUrl.length()  ? newUrl  : String(stationsOverrideUrl(idx)));
    if (!stationsSetSlot(idx, keepName, keepUrl)) {
        outln(F("Save failed (NVS full and nothing droppable). No change."));
        return;
    }
    outln(F("Saved. Use 'station <n>' to play the edited entry."));
}

static void cmdStationReset(const String &arg) {
    int n = arg.toInt();
    if (n < 1 || n > stationsCount()) {
        outln(F("Usage: station reset <n>")); return;
    }
    stationsResetSlot(n - 1);
    Serial.print(F("Slot ")); Serial.print(n); outln(F(" reverted to default."));
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

static void printScanList(int n) {
    for (int i = 0; i < n; i++) {
        const ScanResult *r = netScanResult(i);
        if (i + 1 < 10) Serial.write(' ');
        Serial.print(i + 1); Serial.print(F("  "));
        Serial.print(r->rssi); Serial.print(F(" dBm  "));
        Serial.print(F("ch")); Serial.print(r->channel);
        if (r->channel < 10) Serial.write(' ');
        Serial.print(F("  "));
        Serial.print(encStr(r->encryption)); Serial.print(F("  "));
        outln(r->ssid);
    }
}

static void cmdWifiScan(bool hard = false) {
    outln(hard ? F("Hard scanning (3 passes, Ctrl-C to abort)...") : F("Scanning..."));
    int n = netScanNow();
    if (hard) {
        // Phone hotspots often beacon slowly. A second and third pass,
        // plus a small sleep between, catches APs that missed the first.
        for (int pass = 1; pass < 3; pass++) {
            for (int slept = 0; slept < 500; slept += 20) {
                cliPoll(); if (cliCheckInterrupt()) { outln(F("Aborted.")); return; }
                delay(20);
            }
            n = netScanNow();
        }
    }
    if (n == 0) {
        outln(F("No networks found."));
        outln(F("  * If looking for a phone hotspot:"));
        outln(F("    - set AP band to 2.4 GHz (not 5 GHz)"));
        outln(F("    - security WPA2 or WPA2/3 (not WPA3-only)"));
        outln(F("    - turn the hotspot off and on again to refresh its beacon"));
        return;
    }
    outln();
    printScanList(n);
    outln();
}

static void cmdWifiConnect(const String &rest) {
    // Parse: "SSID" OR "SSID password" OR "\"SSID with spaces\" password".
    String ssid, pass;
    String r = rest; r.trim();
    if (r.length() == 0) { outln(F("Usage: wifi connect <ssid> [password]")); return; }
    if (r.startsWith("\"")) {
        int end = r.indexOf('"', 1);
        if (end < 0) { outln(F("Unterminated quoted SSID.")); return; }
        ssid = r.substring(1, end);
        pass = r.substring(end + 1); pass.trim();
    } else {
        int sp = r.indexOf(' ');
        if (sp < 0) { ssid = r; }
        else        { ssid = r.substring(0, sp); pass = r.substring(sp + 1); pass.trim(); }
    }
    outln();
    Serial.print(F("Connecting to '")); Serial.print(ssid); outln(F("'..."));
    if (netConnectAdhoc(ssid, pass, 12000)) {
        outln(F("Connected."));
        // Offer to save.
        outln(F("Save this network to the saved list? [y/N]"));
        String y = readLineBlocking("> ", false); y.trim();
        if (y.equalsIgnoreCase("y") || y.equalsIgnoreCase("yes")) {
            wifiAddNetwork(ssid, pass);
            outln(F("Saved."));
        }
    } else {
        outln(F("Connect failed. Check SSID / password, and band/security on the AP."));
    }
}

static void cmdWifiList() {
    outln();
    int n = wifiNetworkCount();
    if (n == 0) { outln(F("No saved networks.")); return; }
    int joined = netLastJoinedSlot();
    for (int i = 0; i < n; i++) {
        Serial.print(i == joined ? F(" *") : F("  "));
        Serial.print(F(" ")); Serial.print(i + 1); Serial.print(F(". "));
        Serial.print(wifiNetworkSsid(i));
        size_t plen = strlen(wifiNetworkPass(i));
        if (plen > 0) { Serial.print(F("  (pass: ")); Serial.print(plen); Serial.print(F(" chars)")); }
        else          { Serial.print(F("  (open)")); }
        if (i == joined) Serial.print(F("   [joined]"));
        outln();
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
            // Clean STA state before scanning. If we land here straight after
            // a failed netConnect() the driver is still in mid-association
            // teardown, which has been observed to yield 0 scan results.
            WiFi.disconnect(false, true);
            delay(200);
            int n = netScanNow();
            if (n == 0) { outln(F("No networks found. Enter SSID manually.")); }
            else {
                outln();
                printScanList(n);
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

// Pump the serial CLI while waiting on a long net operation, so Ctrl-C
// is delivered and latched into cliCheckInterrupt().
static bool cliAbortCb() {
    cliPoll();
    return cliCheckInterrupt();
}

static void cmdReconnect() {
    if (wifiNetworkCount() == 0) { outln(F("No saved networks. Run 'wifi add' first.")); return; }
    outln(F("Reconnecting... (Ctrl-C to abort)"));
    WiFi.disconnect(true);
    delay(100);
    netConnect(cliAbortCb, nullptr);
    if (cliCheckInterrupt()) { outln(F("Aborted.")); return; }
    outln(netConnected() ? "OK." : "No saved network connected.");
}

static void cmdSd(const String &arg) {
    String sub, rest;
    splitArg(arg, sub, rest);

    if (sub.length() == 0 || eqi(sub, "status")) {
        Serial.print(F("SD : "));
        outln(storageSdMounted() ? "mounted" : "not mounted");
        if (storageSdMounted()) {
            Serial.print(F("stations.csv : "));
            outln(storageSdExists("/stations.csv") ? "present" : "missing");
            Serial.print(F("theme.ini    : "));
            outln(storageSdExists("/theme.ini") ? "present" : "missing");
        }
        return;
    }
    if (eqi(sub, "ls")) {
        if (!storageSdMounted()) { outln(F("SD not mounted.")); return; }
        String p = rest.length() ? rest : String("/");
        File root = SD_MMC.open(p);
        if (!root || !root.isDirectory()) { outln(F("Not a directory.")); return; }
        File f = root.openNextFile();
        outln();
        while (f) {
            Serial.print(f.isDirectory() ? F("  [DIR] ") : F("        "));
            Serial.print(f.name());
            if (!f.isDirectory()) { Serial.print(F("  ")); Serial.print(f.size()); Serial.print(F(" B")); }
            outln();
            f = root.openNextFile();
        }
        outln();
        return;
    }
    if (eqi(sub, "reload")) {
        if (!storageSdMounted()) { outln(F("SD not mounted.")); return; }
        int n = stationsLoadFromSd();
        bool t = displayLoadThemeFromSd();
        Serial.printf("Reloaded %d stations; theme %s.\r\n", n, t ? "applied" : "unchanged");
        return;
    }
    outln(F("Usage: sd [status | ls <path> | reload]"));
}

// ---- dispatcher ----------------------------------------------------------

static void dispatch(const String &raw) {
    String line = raw; line.trim();
    if (line.length() == 0) return;
    cliClearInterrupt();   // each new command starts with a clean ^C state

    String cmd, arg;
    int sp = line.indexOf(' ');
    if (sp < 0) { cmd = line; }
    else        { cmd = line.substring(0, sp); arg = line.substring(sp + 1); arg.trim(); }

    if      (eqi(cmd, "help") || cmd == "?")                                 cmdHelp();
    else if (eqi(cmd, "status") || eqi(cmd, "stat") || eqi(cmd, "s"))        cmdStatus();
    else if (eqi(cmd, "stations") || eqi(cmd, "list"))                       cmdListStations();
    else if (eqi(cmd, "station") || eqi(cmd, "sel")) {
        String sub, rest;
        splitArg(arg, sub, rest);
        if (eqi(sub, "edit"))        cmdStationEdit(rest);
        else if (eqi(sub, "reset"))  cmdStationReset(rest);
        else                         cmdSelectStation(arg);
    }
    else if (eqi(cmd, "next"))                                               { audioNextStation(); cmdStatus(); }
    else if (eqi(cmd, "prev"))                                               { audioPrevStation(); cmdStatus(); }
    else if (eqi(cmd, "volume") || eqi(cmd, "vol") || eqi(cmd, "v"))         cmdVolume(arg);
    else if (eqi(cmd, "vol+") || cmd == "+")                                 cmdVolBump(+1);
    else if (eqi(cmd, "vol-") || cmd == "-")                                 cmdVolBump(-1);
    else if (eqi(cmd, "wifi")) {
        String sub, rest;
        splitArg(arg, sub, rest);
        if      (sub.length() == 0)        cmdWifiAdd("");
        else if (eqi(sub, "scan"))         cmdWifiScan();
        else if (eqi(sub, "scan-hard") || eqi(sub, "rescan"))
                                            cmdWifiScan(/*hard=*/true);
        else if (eqi(sub, "list"))         cmdWifiList();
        else if (eqi(sub, "show"))         cmdWifiList();   // alias for list
        else if (eqi(sub, "clear"))        cmdWifiClear();
        else if (eqi(sub, "add"))          cmdWifiAdd(rest);
        else if (eqi(sub, "connect") || eqi(sub, "try"))
                                            cmdWifiConnect(rest);
        else if (eqi(sub, "remove") || eqi(sub, "rm") || eqi(sub, "del"))
                                            cmdWifiRemove(rest);
        else if (eqi(sub, "move") || eqi(sub, "mv"))
                                            cmdWifiMove(rest);
        else if (eqi(sub, "diag") || eqi(sub, "info") || eqi(sub, "status"))
                                            netPrintDiag();
        else                                outln(F("Unknown wifi subcommand. Try 'help'."));
    }
    else if (eqi(cmd, "cancel") || eqi(cmd, "exit")) {
        cliCancelSetup();
        outln(F("Setup cancelled (if active)."));
    }
    else if (eqi(cmd, "time")) {
        String sub, rest;
        splitArg(arg, sub, rest);
        if (sub.length() == 0 || eqi(sub, "show")) {
            char d[32], t[16];
            clockFormatDate(d, sizeof(d));
            clockFormatTime(t, sizeof(t));
            Serial.print(F("date  : ")); outln(d[0] ? d : "(not synced)");
            Serial.print(F("time  : ")); outln(t[0] ? t : "(not synced)");
            Serial.print(F("tz    : ")); outln(clockTimezone());
        } else if (eqi(sub, "set")) {
            if (rest.length() == 0) { outln(F("Usage: time set <POSIX TZ string>")); }
            else if (clockSetTimezone(rest.c_str())) { outln(F("Timezone saved.")); }
            else                                       { outln(F("Empty tz.")); }
        } else outln(F("Usage: time [show|set <tz>]"));
    }
    else if (eqi(cmd, "reconnect"))                                          cmdReconnect();
    else if (eqi(cmd, "sd"))                                                 cmdSd(arg);
    else if (eqi(cmd, "log")) {
        String sub, rest;
        splitArg(arg, sub, rest);
        if (sub.length() == 0) {
            Serial.printf("log: audio=%s  verbose=%s  sd=%s\r\n",
                          audioLogEnabled() ? "on" : "off",
                          logVerbose()      ? "on" : "off",
                          logSdEnabled()    ? "on" : "off");
        } else if (eqi(sub, "on"))       { audioSetLogEnabled(true);  outln(F("log audio: on")); }
        else if (eqi(sub, "off"))        { audioSetLogEnabled(false); outln(F("log audio: off")); }
        else if (eqi(sub, "verbose")) {
            if (rest.length() == 0)        { Serial.print(F("log verbose: ")); outln(logVerbose() ? "on" : "off"); }
            else if (eqi(rest, "on"))      { logSetVerbose(true);  outln(F("log verbose: on")); }
            else if (eqi(rest, "off"))     { logSetVerbose(false); outln(F("log verbose: off")); }
            else                             outln(F("Usage: log verbose [on|off]"));
        }
        else if (eqi(sub, "sd")) {
            if (rest.length() == 0 || eqi(rest, "status")) {
                Serial.print(F("log sd: ")); outln(logSdEnabled() ? "on" : "off");
            } else if (eqi(rest, "on")) {
                outln(logSdBegin() ? "log sd: on" : "log sd: FAILED (SD not mounted?)");
            } else if (eqi(rest, "off")) {
                logSdEnd(); outln(F("log sd: off"));
            } else outln(F("Usage: log sd [on|off|status]"));
        }
        else outln(F("Usage: log [on|off|verbose on|off|sd on|off]"));
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
    while (!wifiHasNetworks()) {
        cmdWifiAdd("");
    }
}

void cliWaitForNewNetwork(void (*tickCb)()) {
    g_cancelSetup = false;
    int before = wifiNetworkCount();
    outln();
    outln(F("Setup mode: run 'wifi add', 'wifi connect <ssid> [pass]',"));
    outln(F("or 'cancel' to resume without a new network."));
    prompt();
    while (wifiNetworkCount() == before && !g_cancelSetup) {
        cliPoll();
        provisionPoll();
        if (tickCb) tickCb();
        delay(10);
    }
}

void cliPoll() {
    while (Serial.available()) {
        displayNoteActivity();  // wake the panel on any keystroke
        char c = Serial.read();
        if (c == 3) {                   // Ctrl-C: latch interrupt flag
            g_ctrlC = true;
            Serial.print("^C\r\n");
            g_buf = "";
            prompt();
            continue;
        }
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
