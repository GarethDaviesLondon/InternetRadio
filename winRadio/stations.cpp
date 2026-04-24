#include "stations.h"
#include "storage.h"

#include <Arduino.h>

namespace {
struct Preset { const char *name; const char *url; };

// Compiled-in presets. Used once, on first boot, when NVS has no
// "cnt" key and SD has no /stations.csv. After that everything is
// edited in NVS.
const Preset kDefaults[] = {
    { "SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3"        },
    { "Disco Diamond",       "https://discodiamond.radioca.st/autodj"           },
    { "Radio King 175279",   "https://listen.radioking.com/radio/175279/stream/216784" },
    { "Radio Caroline",      "http://sc6.radiocaroline.net:8040/stream"         },
    { "Raute Musik Club",    "https://club-high.rautemusik.fm/;"                },
    { "WGMC Jazz",           "http://greece-media.monroe.edu/wgmc.mp3"          },
    { "Radio Banovina",      "https://audio.radio-banovina.hr:9998/;"           },
    { "Radio Paradise",      "http://stream.radioparadise.com/mp3-128"          },
};
constexpr int kDefaultCount = sizeof(kDefaults) / sizeof(kDefaults[0]);

// RAM-resident active list. Contiguous -- s_list[0..s_count-1] is the
// full active list. Beyond s_count the slots are unused.
constexpr int kMaxStations = 30;
struct Entry { String name; String url; };
Entry s_list[kMaxStations];
int   s_count = 0;

String kN(int i) { return String("na") + i; }
String kU(int i) { return String("ur") + i; }

// Persist s_count and every na<i>/ur<i> pair. Indices >= s_count get
// their keys removed so deletes shrink the NVS footprint.
bool persistAll() {
    bool ok = storagePutInt("stations", "cnt", s_count);
    for (int i = 0; i < kMaxStations; i++) {
        if (i < s_count) {
            ok = ok && storagePutString("stations", kN(i).c_str(), s_list[i].name);
            ok = ok && storagePutString("stations", kU(i).c_str(), s_list[i].url);
        } else {
            storageRemove("stations", kN(i).c_str());
            storageRemove("stations", kU(i).c_str());
        }
    }
    return ok;
}

bool persistOne(int i) {
    if (i < 0 || i >= s_count) return false;
    bool ok = storagePutString("stations", kN(i).c_str(), s_list[i].name);
    ok = ok && storagePutString("stations", kU(i).c_str(), s_list[i].url);
    ok = ok && storagePutInt   ("stations", "cnt", s_count);
    return ok;
}

void seedFromDefaults() {
    s_count = 0;
    for (int i = 0; i < kDefaultCount && i < kMaxStations; i++) {
        s_list[s_count].name = kDefaults[i].name;
        s_list[s_count].url  = kDefaults[i].url;
        s_count++;
    }
    persistAll();
    Serial.printf("stations: seeded %d compiled defaults\r\n", s_count);
}

// Read CSV text and fill s_list. Caller handles persistence.
int parseCsv(const String &text) {
    int count = 0;
    int start = 0;
    while (start < (int)text.length() && count < kMaxStations) {
        int nl = text.indexOf('\n', start);
        String line = (nl < 0) ? text.substring(start) : text.substring(start, nl);
        start = (nl < 0) ? text.length() : nl + 1;
        if (line.endsWith("\r")) line.remove(line.length() - 1);
        line.trim();
        if (line.length() == 0 || line.startsWith("#") || line.startsWith(";")) continue;
        int comma = line.indexOf(',');
        if (comma <= 0) continue;
        String name = line.substring(0, comma); name.trim();
        String url  = line.substring(comma + 1); url.trim();
        if (url.length() == 0) continue;
        s_list[count].name = name.length() ? name : url;
        s_list[count].url  = url;
        count++;
    }
    return count;
}
} // namespace

int stationsMax()   { return kMaxStations; }
int stationsCount() { return s_count; }

const char *stationsName(int idx) {
    if (idx < 0 || idx >= s_count) return "";
    return s_list[idx].name.c_str();
}
const char *stationsUrl(int idx) {
    if (idx < 0 || idx >= s_count) return "";
    return s_list[idx].url.c_str();
}

void stationsBegin() {
    // If NVS already has a count, restore from there. "cnt" uses -1 as
    // the "never set" sentinel so 0 (= empty list) is a valid state.
    int n = storageGetInt("stations", "cnt", -1);
    if (n >= 0) {
        if (n > kMaxStations) n = kMaxStations;
        s_count = 0;
        for (int i = 0; i < n; i++) {
            String nm = storageGetString("stations", kN(i).c_str(), "");
            String ur = storageGetString("stations", kU(i).c_str(), "");
            if (ur.length() == 0) continue;   // skip corrupt rows
            s_list[s_count].name = nm.length() ? nm : ur;
            s_list[s_count].url  = ur;
            s_count++;
        }
        Serial.printf("stations: loaded %d from NVS\r\n", s_count);
        return;
    }

    // First boot: try SD CSV, fall back to compiled defaults.
    if (stationsLoadFromSd() > 0) return;
    seedFromDefaults();
}

bool stationsEdit(int idx, const String &name, const String &url) {
    if (idx < 0 || idx >= s_count) return false;
    s_list[idx].name = name.length() ? name : url;
    s_list[idx].url  = url;
    return persistOne(idx);
}

int stationsAdd(const String &name, const String &url) {
    if (s_count >= kMaxStations) return -1;
    if (url.length() == 0)       return -1;
    int idx = s_count;
    s_list[idx].name = name.length() ? name : url;
    s_list[idx].url  = url;
    s_count++;
    if (!persistOne(idx)) {
        s_count--;
        s_list[idx].name = "";
        s_list[idx].url  = "";
        return -1;
    }
    return idx;
}

bool stationsDelete(int idx) {
    if (idx < 0 || idx >= s_count) return false;
    for (int i = idx; i < s_count - 1; i++) s_list[i] = s_list[i + 1];
    s_count--;
    s_list[s_count].name = "";
    s_list[s_count].url  = "";
    return persistAll();
}

void stationsResetToDefaults() {
    s_count = 0;
    if (stationsLoadFromSd() > 0) return;
    seedFromDefaults();
}

int stationsLoadFromSd(const char *path) {
    if (!storageSdExists(path)) return 0;
    String text;
    if (!storageSdReadText(path, text, 16384)) return 0;
    int n = parseCsv(text);
    if (n == 0) return 0;
    s_count = n;
    persistAll();
    Serial.printf("stations: loaded %d from %s\r\n", s_count, path);
    return s_count;
}
