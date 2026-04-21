#include "storage.h"
#include <Preferences.h>

namespace {
Preferences g_prefs;

struct Network { String ssid; String pass; };
Network  g_nets[kWifiMaxNetworks];
int      g_count   = 0;
bool     g_loaded  = false;
bool     g_sdMounted = false;

String slotKeyS(int i) { return String("s") + i; }
String slotKeyP(int i) { return String("p") + i; }

// Persist the full list to NVS. Simple and infrequent (only on add /
// remove / reorder / clear), so rewriting everything is fine.
void persistAll() {
    g_prefs.begin("wifi", false);
    g_prefs.putInt("n", g_count);
    for (int i = 0; i < kWifiMaxNetworks; i++) {
        String sk = slotKeyS(i), pk = slotKeyP(i);
        if (i < g_count) {
            g_prefs.putString(sk.c_str(), g_nets[i].ssid);
            g_prefs.putString(pk.c_str(), g_nets[i].pass);
        } else {
            g_prefs.remove(sk.c_str());
            g_prefs.remove(pk.c_str());
        }
    }
    g_prefs.end();
}

// Copy legacy `ssid`/`pass` into slot 0 if present and the list is empty,
// then wipe the legacy keys. Called once on first load.
void migrateLegacy() {
    g_prefs.begin("wifi", false);
    if (g_prefs.isKey("ssid") || g_prefs.isKey("pass")) {
        String oldSsid = g_prefs.getString("ssid", "");
        String oldPass = g_prefs.getString("pass", "");
        g_prefs.remove("ssid");
        g_prefs.remove("pass");
        g_prefs.end();
        if (oldSsid.length() > 0 && g_count < kWifiMaxNetworks) {
            g_nets[g_count].ssid = oldSsid;
            g_nets[g_count].pass = oldPass;
            g_count++;
            persistAll();
        }
        return;
    }
    g_prefs.end();
}

int findBySsid(const String &ssid) {
    for (int i = 0; i < g_count; i++)
        if (g_nets[i].ssid == ssid) return i;
    return -1;
}
} // namespace

// ---- Generic typed accessors --------------------------------------------

String storageGetString(const char *ns, const char *key, const String &dflt) {
    g_prefs.begin(ns, true);
    String v = g_prefs.getString(key, dflt);
    g_prefs.end();
    return v;
}
bool storagePutString(const char *ns, const char *key, const String &value) {
    g_prefs.begin(ns, false);
    bool ok = g_prefs.putString(key, value) > 0;
    g_prefs.end();
    return ok;
}
int32_t storageGetInt(const char *ns, const char *key, int32_t dflt) {
    g_prefs.begin(ns, true);
    int32_t v = g_prefs.getInt(key, dflt);
    g_prefs.end();
    return v;
}
bool storagePutInt(const char *ns, const char *key, int32_t value) {
    g_prefs.begin(ns, false);
    bool ok = g_prefs.putInt(key, value) > 0;
    g_prefs.end();
    return ok;
}
bool storageRemove(const char *ns, const char *key) {
    g_prefs.begin(ns, false);
    bool ok = g_prefs.remove(key);
    g_prefs.end();
    return ok;
}

// ---- WiFi credential list -----------------------------------------------

void wifiLoadNetworks() {
    if (g_loaded) return;
    g_prefs.begin("wifi", true);
    g_count = g_prefs.getInt("n", 0);
    if (g_count < 0) g_count = 0;
    if (g_count > kWifiMaxNetworks) g_count = kWifiMaxNetworks;
    for (int i = 0; i < g_count; i++) {
        g_nets[i].ssid = g_prefs.getString(slotKeyS(i).c_str(), "");
        g_nets[i].pass = g_prefs.getString(slotKeyP(i).c_str(), "");
    }
    g_prefs.end();
    migrateLegacy();
    g_loaded = true;
}

int  wifiNetworkCount() { return g_count; }
bool wifiHasNetworks()  { return g_count > 0; }

const char *wifiNetworkSsid(int idx) {
    if (idx < 0 || idx >= g_count) return "";
    return g_nets[idx].ssid.c_str();
}
const char *wifiNetworkPass(int idx) {
    if (idx < 0 || idx >= g_count) return "";
    return g_nets[idx].pass.c_str();
}

bool wifiAddNetwork(const String &ssid, const String &pass) {
    if (ssid.length() == 0) return false;
    int existing = findBySsid(ssid);
    if (existing >= 0) {
        // Update password in place; preserve ordering.
        g_nets[existing].pass = pass;
        persistAll();
        return true;
    }
    if (g_count >= kWifiMaxNetworks) return false;
    g_nets[g_count].ssid = ssid;
    g_nets[g_count].pass = pass;
    g_count++;
    persistAll();
    return true;
}

bool wifiRemoveNetwork(int idx) {
    if (idx < 0 || idx >= g_count) return false;
    for (int i = idx; i < g_count - 1; i++) g_nets[i] = g_nets[i + 1];
    g_nets[g_count - 1].ssid = "";
    g_nets[g_count - 1].pass = "";
    g_count--;
    persistAll();
    return true;
}

bool wifiMoveNetwork(int from, int to) {
    if (from < 0 || from >= g_count) return false;
    if (to   < 0 || to   >= g_count) return false;
    if (from == to) return true;
    Network tmp = g_nets[from];
    if (from < to) {
        for (int i = from; i < to; i++) g_nets[i] = g_nets[i + 1];
    } else {
        for (int i = from; i > to; i--) g_nets[i] = g_nets[i - 1];
    }
    g_nets[to] = tmp;
    persistAll();
    return true;
}

void wifiClearAllNetworks() {
    for (int i = 0; i < kWifiMaxNetworks; i++) {
        g_nets[i].ssid = "";
        g_nets[i].pass = "";
    }
    g_count = 0;
    persistAll();
}

// ---- SD card (stub) ------------------------------------------------------
// TODO(SD): SD_MMC pins on this board are clk=16 cmd=15 d0=17 d1=18 d2=13
// d3=14 (from Volos's original sketch). Wire SD_MMC.setPins() + .begin() here.

bool storageSdMount()    { return false; }
bool storageSdMounted()  { return g_sdMounted; }
void storageSdUnmount()  { g_sdMounted = false; }
