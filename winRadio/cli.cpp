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
#include "discover.h"

#include <vector>

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
    int escState = 0;   // 0=normal, 1=saw ESC, 2=inside CSI (see cliPoll)
    while (true) {
        if (Serial.available()) {
            char c = Serial.read();

            // Skip multi-byte escape sequences (Delete, arrows, etc.).
            if (escState == 1) {
                if (c == '[' || c == 'O') escState = 2;
                else                       escState = 0;
                continue;
            }
            if (escState == 2) {
                if ((unsigned char)c >= 0x40 && (unsigned char)c <= 0x7E) escState = 0;
                continue;
            }
            if (c == 0x1B) { escState = 1; continue; }

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
    outln(F("  stations, list         List saved stations"));
    outln(F("  station <n>, sel <n>   Select station N (1-based)"));
    outln(F("  station edit <n>       Edit friendly name + URL for slot N"));
    outln(F("  station add            Append a new station (interactive)"));
    outln(F("  station del <n>        Delete slot N (list shifts down)"));
    outln(F("  station reset-all      Wipe list and reseed defaults"));
    outln(F("  next / prev            Cycle stations"));
    outln();
    outln(F("  find <text>            Search Radio-Browser for stations"));
    outln(F("  find tag <tag>         Search by genre / tag"));
    outln(F("  find country <name>    Search by country"));
    outln(F("  find list              Show the last search's results"));
    outln(F("  find play <n>          Preview hit N (no save)"));
    outln(F("  find save <n>          Add hit N to the station list"));
    outln(F("  volume [n], vol [n]    Show or set volume (1..5)"));
    outln(F("  vol+ / vol- / + / -    Step volume"));
    outln();
    outln(F("  wifi scan              Scan visible networks (shows channel)"));
    outln(F("  wifi scan-hard         3-pass scan -- catches slow-beacon hotspots"));
    outln(F("  wifi list              List saved networks (ordered)"));
    outln(F("  wifi add [ssid]        Add a network (interactive: scan + pick + pass)"));
    outln(F("  wifi connect <ssid> [pass]"));
    outln(F("                         Connects (uses saved creds if known); offers"));
    outln(F("                         to save on success, prompt to update on fail"));
    outln(F("  wifi                   Alias for 'wifi add'"));
    outln(F("  wifi remove <n>        Remove saved network N"));
    outln(F("  wifi move <from> <to>  Reorder saved networks"));
    outln(F("  wifi clear             Erase ALL saved networks"));
    outln(F("  wifi diag              Dump WiFi driver state + last reason code"));
    outln(F("  cancel                 Exit setup-mode and resume boot flow"));
    outln(F("  reconnect              Try saved networks again"));
    outln(F("  captive                Probe for a captive portal on this network"));
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
    outln(F("Leave a field blank to keep the current value."));
    String cur = stationsName(idx);
    String curUrl = stationsUrl(idx);
    String newName = readLineBlocking("Friendly name: ", false); newName.trim();
    String newUrl  = readLineBlocking("URL          : ", false); newUrl.trim();
    String keepName = (newName.length() ? newName : cur);
    String keepUrl  = (newUrl.length()  ? newUrl  : curUrl);
    if (!stationsEdit(idx, keepName, keepUrl)) {
        outln(F("Save failed (NVS full). No change."));
        return;
    }
    outln(F("Saved."));
}

static void cmdStationAdd() {
    if (stationsCount() >= stationsMax()) {
        Serial.print(F("List is full (")); Serial.print(stationsMax());
        outln(F(" entries). Delete one first."));
        return;
    }
    outln();
    String newName = readLineBlocking("Friendly name: ", false); newName.trim();
    String newUrl  = readLineBlocking("URL          : ", false); newUrl.trim();
    if (newUrl.length() == 0) { outln(F("URL is required.")); return; }
    int idx = stationsAdd(newName, newUrl);
    if (idx < 0) { outln(F("Add failed (NVS full or list full).")); return; }
    Serial.print(F("Added as slot ")); Serial.println(idx + 1);
}

static void cmdStationDel(const String &arg) {
    int n = arg.toInt();
    if (n < 1 || n > stationsCount()) {
        outln(F("Usage: station del <n>")); return;
    }
    int wasCurrent = audioCurrentStation();
    if (!stationsDelete(n - 1)) { outln(F("Delete failed.")); return; }
    Serial.print(F("Deleted slot ")); Serial.println(n);
    // If we deleted the playing slot, fall back to slot 0 (if any).
    if (wasCurrent == n - 1 && stationsCount() > 0) audioSelectStation(0);
}

static void cmdStationResetAll() {
    stationsResetToDefaults();
    Serial.print(F("Station list reseeded. Now ")); Serial.print(stationsCount());
    outln(F(" entries."));
}

// Cached results from the most recent CLI search, so "find save <n>" /
// "find play <n>" can refer back to them by 1-based index.
static std::vector<DiscoverHit> g_findHits;
static String                    g_findQuery;

static void printFindUsage() {
    outln();
    outln(F("Usage:"));
    outln(F("  find <text>             search station name (Radio-Browser)"));
    outln(F("  find tag <tag>          search by genre/tag"));
    outln(F("  find country <name>     search by country"));
    outln(F("  find list               list last search results"));
    outln(F("  find play <n>           preview hit N (does NOT save)"));
    outln(F("  find save <n>           append hit N to the station list"));
    outln();
}

static void printFindHits() {
    if (g_findHits.empty()) { outln(F("No results. Run a search first.")); return; }
    outln();
    Serial.print(F("Last query: ")); outln(g_findQuery.c_str());
    for (size_t i = 0; i < g_findHits.size(); i++) {
        const auto &h = g_findHits[i];
        if (i + 1 < 10) Serial.write(' ');
        Serial.print(i + 1); Serial.print(F(". "));
        Serial.print(h.name);
        Serial.print(F("   ["));
        if (h.bitrate > 0) { Serial.print(h.bitrate); Serial.print(F(" kbps ")); }
        Serial.print(h.codec);
        if (h.country.length()) { Serial.print(F(" / ")); Serial.print(h.country); }
        outln(F("]"));
        Serial.print(F("       ")); outln(h.url.c_str());
    }
    outln();
}

static void runFindSearch(const String &q, const String &tag, const String &country) {
    g_findQuery = "name='" + q + "' tag='" + tag + "' country='" + country + "'";
    g_findHits.clear();
    Serial.print(F("Searching... "));
    int n = discoverSearch(q, tag, country, 25, g_findHits);
    outln();
    if (n < 0) {
        Serial.print(F("Search failed: ")); outln(discoverLastError()); return;
    }
    if (g_findHits.empty()) {
        outln(F("No matches. Try a different term or 'find tag <genre>'."));
        return;
    }
    printFindHits();
    outln(F("Use 'find save <n>' to add a hit to the station list."));
    outln(F("Use 'find play <n>' to preview without saving."));
}

static void cmdFind(const String &arg) {
    String a = arg; a.trim();
    if (a.length() == 0) { printFindUsage(); return; }

    String sub, rest;
    splitArg(a, sub, rest);

    if (eqi(sub, "list"))           { printFindHits(); return; }
    if (eqi(sub, "tag"))            { runFindSearch("", rest, "");     return; }
    if (eqi(sub, "country"))        { runFindSearch("", "",   rest);   return; }

    if (eqi(sub, "save") || eqi(sub, "play")) {
        int n = rest.toInt();
        if (n < 1 || n > (int)g_findHits.size()) {
            Serial.print(F("Index must be 1..")); Serial.println(g_findHits.size());
            return;
        }
        const auto &h = g_findHits[n - 1];
        if (eqi(sub, "save")) {
            int idx = stationsAdd(h.name, h.url);
            if (idx < 0) {
                outln(F("Save failed (list full or NVS full)."));
                return;
            }
            Serial.print(F("Added as slot ")); Serial.println(idx + 1);
        } else {
            Serial.print(F("Previewing: ")); outln(h.url.c_str());
            audioPlayAdhoc(h.url.c_str(), h.name.c_str());
        }
        return;
    }

    // Anything else: treat the whole arg as a free-text name search.
    runFindSearch(a, "", "");
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

// Look up a stored password by SSID; empty string if not saved.
static String storedPassFor(const String &ssid) {
    int n = wifiNetworkCount();
    for (int i = 0; i < n; i++) {
        if (ssid.equalsIgnoreCase(wifiNetworkSsid(i))) {
            return String(wifiNetworkPass(i));
        }
    }
    return String();
}

// Progress dots during a connect. Prints one dot every ~750 ms; flushes
// the line on success/fail (caller does that).
static void cliWifiProgress(uint32_t elapsedMs) {
    static uint32_t lastDot = 0;
    if (lastDot == 0 || elapsedMs - lastDot >= 750) {
        Serial.print('.');
        lastDot = elapsedMs;
    }
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

    // If the user didn't supply a password, look up stored credentials
    // for this SSID. If none, try with empty password (open auth) on
    // the first attempt; we'll prompt for one if that fails.
    bool usingStored = false;
    if (pass.length() == 0) {
        String stored = storedPassFor(ssid);
        if (stored.length() > 0) {
            pass = stored;
            usingStored = true;
        }
    }

    outln();
    Serial.print(F("Connecting to '")); Serial.print(ssid); Serial.print(F("'"));
    if (usingStored) Serial.print(F(" (using saved password)"));
    Serial.print(F(" "));
    bool ok = netConnectAdhoc(ssid, pass, 12000, cliWifiProgress);
    outln();

    // If the first attempt failed with no password, give the user a
    // chance to type one and retry without losing the SSID they typed.
    if (!ok && pass.length() == 0) {
        outln(F("Connect failed without a password. The network probably needs one."));
        String entered = readLineBlocking("Password (blank to abort): ", false);
        entered.trim();
        if (entered.length()) {
            Serial.print(F("Retrying with password "));
            ok = netConnectAdhoc(ssid, entered, 12000, cliWifiProgress);
            outln();
            if (ok) pass = entered;
        }
    }
    // If the stored password failed, give a chance to update it.
    else if (!ok && usingStored) {
        outln(F("The saved password didn't work. The AP password may have changed."));
        String entered = readLineBlocking("New password (blank to abort): ", false);
        entered.trim();
        if (entered.length()) {
            Serial.print(F("Retrying with new password "));
            ok = netConnectAdhoc(ssid, entered, 12000, cliWifiProgress);
            outln();
            if (ok) {
                pass = entered;
                wifiAddNetwork(ssid, pass);   // updates in-place if SSID already saved
                outln(F("Saved password updated."));
            }
        }
    }

    if (!ok) {
        outln(F("Connect failed. Check SSID / password, and band/security on the AP."));
        return;
    }

    outln(F("Connected."));
    // Captive-portal probe so the user finds out NOW rather than when
    // audio fails to start. Surfaces on the LCD too if a portal is
    // detected -- handy when the radio is across the room.
    Serial.print(F("Captive probe: "));
    CaptiveStatus cs = netCheckCaptive();
    Serial.println(netCaptiveStatusName(cs));
    if (cs == CAPTIVE_PORTAL) {
        Serial.print(F("Portal URL: ")); outln(netCaptivePortalUrl());
        outln(F("Open that URL on a phone on the same WiFi and sign in."));
        displayCaptiveOpen(ssid.c_str(), netCaptivePortalUrl());
        if (!provisionActive()) {
            provisionStartBackground();
            outln(F("Setup AP brought up so you can change networks if needed."));
        }
    }
    if (!usingStored) {
        // Offer to save (or update the stored password).
        outln(F("Save this network to the saved list? [y/N]"));
        String y = readLineBlocking("> ", false); y.trim();
        if (y.equalsIgnoreCase("y") || y.equalsIgnoreCase("yes")) {
            wifiAddNetwork(ssid, pass);
            outln(F("Saved."));
        }
    }
    // Promote: the user explicitly chose this network NOW, so it
    // should win on the next boot. Find its slot and bump to top.
    for (int i = 0; i < wifiNetworkCount(); i++) {
        if (ssid.equalsIgnoreCase(wifiNetworkSsid(i))) {
            if (i != 0) {
                wifiPromoteNetwork(i);
                outln(F("Promoted to top of the boot-walk order."));
            }
            break;
        }
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

// Probe for a captive portal. If found, also surfaces it on the LCD
// so the user can grab their phone and complete the login. Background
// loop in winRadio.ino retries every 15 s while the screen is up.
static void cmdCaptive() {
    if (!netConnected()) { outln(F("Not associated.")); return; }
    Serial.print(F("Probing... "));
    CaptiveStatus cs = netCheckCaptive();
    Serial.println(netCaptiveStatusName(cs));
    if (cs == CAPTIVE_PORTAL) {
        Serial.print(F("Portal URL: "));
        outln(netCaptivePortalUrl());
        outln(F("Open that URL on a phone connected to the same WiFi"));
        outln(F("and complete the login. The radio will retry every 15 s."));
        // Surface it on the display too.
        String ssid = netCurrentSsid();
        displayCaptiveOpen(ssid.c_str(), netCaptivePortalUrl());
        if (!provisionActive()) {
            provisionStartBackground();
            outln(F("Setup AP brought up so you can change networks if needed."));
        }
    } else if (cs == CAPTIVE_ONLINE) {
        outln(F("Internet looks open."));
    } else if (cs == CAPTIVE_OFFLINE) {
        outln(F("Probe failed (no DNS / no route)."));
    }
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
        if      (eqi(sub, "edit"))       cmdStationEdit(rest);
        else if (eqi(sub, "add"))        cmdStationAdd();
        else if (eqi(sub, "del") ||
                 eqi(sub, "delete"))     cmdStationDel(rest);
        else if (eqi(sub, "reset-all") ||
                 eqi(sub, "resetall"))   cmdStationResetAll();
        else                             cmdSelectStation(arg);
    }
    else if (eqi(cmd, "find") || eqi(cmd, "search"))                         cmdFind(arg);
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
    else if (eqi(cmd, "captive"))                                            cmdCaptive();
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
    // Tiny ANSI / VT escape skipper. PuTTY (and most terminals) send
    // multi-byte sequences for Delete, arrow keys, F-keys, etc. The
    // first byte is ESC (0x1B), normally followed by '[' (CSI). The
    // sequence ends at a "final byte" in the range 0x40..0x7E.
    // Without this filter the printable bytes inside the sequence
    // (e.g. '[', '3', '~' for Delete) leak into the command buffer
    // and mangle the typed line.
    static int s_escState = 0;   // 0=normal, 1=saw ESC, 2=inside CSI

    while (Serial.available()) {
        displayNoteActivity();  // wake the panel on any keystroke
        char c = Serial.read();

        // Escape-sequence skip path.
        if (s_escState == 1) {
            // Right after ESC: '[' enters CSI; anything else cancels.
            if (c == '[' || c == 'O') s_escState = 2;
            else                       s_escState = 0;
            continue;
        }
        if (s_escState == 2) {
            // Final byte of a CSI sequence -- end the skip.
            if ((unsigned char)c >= 0x40 && (unsigned char)c <= 0x7E) s_escState = 0;
            continue;
        }
        if (c == 0x1B) { s_escState = 1; continue; }

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
