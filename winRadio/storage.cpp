#include "storage.h"
#include <Preferences.h>

static Preferences g_prefs;
static String      g_ssid = "";
static String      g_pass = "";
static bool        g_sdMounted = false;

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

// ---- WiFi credential convenience ----------------------------------------

bool storageLoadWifiCreds() {
    g_ssid = storageGetString("wifi", "ssid", "");
    g_pass = storageGetString("wifi", "pass", "");
    return g_ssid.length() > 0;
}

bool        storageHasWifiCreds() { return g_ssid.length() > 0; }
const char *storageWifiSsid()     { return g_ssid.c_str(); }
const char *storageWifiPassword() { return g_pass.c_str(); }

void storageSaveWifiCreds(const String &ssid, const String &pass) {
    storagePutString("wifi", "ssid", ssid);
    storagePutString("wifi", "pass", pass);
    g_ssid = ssid;
    g_pass = pass;
}

void storageClearWifiCreds() {
    storageRemove("wifi", "ssid");
    storageRemove("wifi", "pass");
    g_ssid = "";
    g_pass = "";
}

// ---- SD card (stub) ------------------------------------------------------
// TODO(SD): SD_MMC pins on this board are clk=16 cmd=15 d0=17 d1=18 d2=13
// d3=14 (from Volos's original sketch). Wire SD_MMC.setPins() + .begin()
// here. For now, mount returns false so callers can fall back gracefully.

bool storageSdMount()    { return false; }
bool storageSdMounted()  { return g_sdMounted; }
void storageSdUnmount()  { g_sdMounted = false; }
