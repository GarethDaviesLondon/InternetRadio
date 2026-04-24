#include "discover.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>

// A few Radio-Browser server nodes we can round-robin over. The API DNS
// hostname (all.api.radio-browser.info) is load-balanced, but individual
// regional mirrors are a useful fallback if DNS is flaky.
static const char *kApiHosts[] = {
    "https://de2.api.radio-browser.info",
    "https://de1.api.radio-browser.info",
    "https://at1.api.radio-browser.info",
    "https://all.api.radio-browser.info",
};
constexpr int kApiHostCount = sizeof(kApiHosts) / sizeof(kApiHosts[0]);

static String s_lastError = "";

// Cached lookup tables (populated on first call).
static std::vector<String> s_tagsCache;
static std::vector<String> s_countriesCache;

const char *discoverLastError() { return s_lastError.c_str(); }

// URL-encode the subset we actually pass: letters, digits, space, a few
// symbols. Everything else becomes %XX. Defensive enough for search
// terms and 2-letter country / tag names.
static String urlEncode(const String &s) {
    String out; out.reserve(s.length());
    static const char *hex = "0123456789ABCDEF";
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else if (c == ' ') {
            out += "%20";
        } else {
            out += '%';
            out += hex[(uint8_t)c >> 4];
            out += hex[(uint8_t)c & 0x0F];
        }
    }
    return out;
}

// Perform a GET and stuff the body into `body`. Returns HTTP status code
// or a negative HTTPClient error. Tries each API host in turn.
static int httpGet(const String &path, String &body) {
    if (WiFi.status() != WL_CONNECTED) { s_lastError = "WiFi disconnected"; return -1; }
    for (int i = 0; i < kApiHostCount; i++) {
        HTTPClient http;
        http.setUserAgent("ON8CIT-WebRadio/0.3");
        http.setTimeout(6000);
        String url = String(kApiHosts[i]) + path;
        if (!http.begin(url)) { s_lastError = "begin failed: " + url; continue; }
        int code = http.GET();
        if (code == 200) {
            body = http.getString();
            http.end();
            return 200;
        }
        s_lastError = "HTTP " + String(code) + " from " + url;
        http.end();
        // Any hard failure -> try next host.
    }
    return -1;
}

// --- Tiny JSON helpers. We don't need a full parser: Radio-Browser
// responses are a flat JSON array of objects with known keys. For each
// object we walk to "key":"value" pairs and copy the interesting fields
// out. Handles escapes (\n, \", \\, \/, \u00XX clipped to ASCII) but
// ignores nested arrays / objects -- none appear in the fields we read.

static size_t skipWs(const String &s, size_t i) {
    while (i < s.length()) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') i++;
        else break;
    }
    return i;
}

// Parse a JSON string starting at `i` (s[i] == '"'). Returns the closing
// quote index + 1 (or String::NPOS on malformed); fills `out`.
static size_t parseString(const String &s, size_t i, String &out) {
    if (i >= s.length() || s[i] != '"') return (size_t)-1;
    i++;
    out = "";
    while (i < s.length()) {
        char c = s[i];
        if (c == '"') return i + 1;
        if (c == '\\' && i + 1 < s.length()) {
            char e = s[i+1];
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u':
                    if (i + 5 < s.length()) {
                        // Trivial unicode escape: accept only ASCII range.
                        int code = strtol(s.substring(i+2, i+6).c_str(), nullptr, 16);
                        if (code > 0 && code < 128) out += (char)code;
                        else                         out += '?';
                        i += 4;
                    }
                    break;
                default: out += e; break;
            }
            i += 2;
            continue;
        }
        out += c;
        i++;
    }
    return (size_t)-1;
}

// Skip a JSON value (string, number, bool, null, array, object) starting
// at `i`. Returns index after the value.
static size_t skipValue(const String &s, size_t i);

static size_t skipContainer(const String &s, size_t i, char open, char close) {
    if (s[i] != open) return (size_t)-1;
    i++;
    int depth = 1;
    while (i < s.length() && depth > 0) {
        char c = s[i];
        if (c == '"') {
            String tmp;
            i = parseString(s, i, tmp);
            if (i == (size_t)-1) return (size_t)-1;
            continue;
        }
        if (c == open)       depth++;
        else if (c == close) depth--;
        i++;
    }
    return i;
}

static size_t skipValue(const String &s, size_t i) {
    i = skipWs(s, i);
    if (i >= s.length()) return (size_t)-1;
    char c = s[i];
    if (c == '"') { String tmp; return parseString(s, i, tmp); }
    if (c == '{') return skipContainer(s, i, '{', '}');
    if (c == '[') return skipContainer(s, i, '[', ']');
    // Primitive: read until next , } ] or whitespace at depth 0.
    while (i < s.length()) {
        char x = s[i];
        if (x == ',' || x == '}' || x == ']' || x == ' ' || x == '\t'
            || x == '\r' || x == '\n') break;
        i++;
    }
    return i;
}

// Parse one Radio-Browser station object into a DiscoverHit. `i` points
// at the '{'. Returns index after the matching '}' or NPOS on error.
static size_t parseHit(const String &s, size_t i, DiscoverHit &h) {
    i = skipWs(s, i);
    if (i >= s.length() || s[i] != '{') return (size_t)-1;
    i++;
    h.name = h.url = h.homepage = h.favicon = h.tags = h.country = h.codec = "";
    h.bitrate = 0;
    while (i < s.length()) {
        i = skipWs(s, i);
        if (i >= s.length()) return (size_t)-1;
        if (s[i] == '}') return i + 1;

        String key;
        i = parseString(s, i, key);
        if (i == (size_t)-1) return (size_t)-1;
        i = skipWs(s, i);
        if (i >= s.length() || s[i] != ':') return (size_t)-1;
        i++;
        i = skipWs(s, i);

        // Only a handful of keys matter. For everything else, skip the
        // value cheaply.
        bool handled = false;
        if (s[i] == '"') {
            String val;
            size_t j = parseString(s, i, val);
            if (j == (size_t)-1) return (size_t)-1;
            if      (key == "name")         { h.name     = val; handled = true; }
            else if (key == "url_resolved") { h.url      = val; handled = true; }
            else if (key == "url" && h.url == "") { h.url = val; handled = true; }
            else if (key == "homepage")     { h.homepage = val; handled = true; }
            else if (key == "favicon")      { h.favicon  = val; handled = true; }
            else if (key == "tags")         { h.tags     = val; handled = true; }
            else if (key == "country")      { h.country  = val; handled = true; }
            else if (key == "codec")        { h.codec    = val; handled = true; }
            i = j;
        } else {
            if (key == "bitrate") {
                // Read an integer primitive.
                size_t j = i;
                while (j < s.length() && (isdigit(s[j]) || s[j] == '.' || s[j] == '-')) j++;
                h.bitrate = (int)atol(s.substring(i, j).c_str());
                i = j;
                handled = true;
            } else {
                i = skipValue(s, i);
                if (i == (size_t)-1) return (size_t)-1;
            }
        }
        (void)handled;

        i = skipWs(s, i);
        if (i < s.length() && s[i] == ',') { i++; continue; }
    }
    return (size_t)-1;
}

int discoverSearch(const String &query, const String &tag,
                   const String &country, int limit,
                   std::vector<DiscoverHit> &out) {
    out.clear();
    if (limit <= 0) limit = 30;
    if (limit > 100) limit = 100;
    String path = "/json/stations/search?order=votes&reverse=true&hidebroken=true&limit=";
    path += limit;
    if (query.length())   path += "&name=" + urlEncode(query);
    if (tag.length())     path += "&tag="  + urlEncode(tag);
    if (country.length()) path += "&country=" + urlEncode(country);

    String body;
    int code = httpGet(path, body);
    if (code != 200) return 0;

    size_t i = 0;
    i = skipWs(body, i);
    if (i >= body.length() || body[i] != '[') {
        s_lastError = "unexpected response (no JSON array)";
        return 0;
    }
    i++;
    while (i < body.length()) {
        i = skipWs(body, i);
        if (i >= body.length() || body[i] == ']') break;
        DiscoverHit h;
        i = parseHit(body, i, h);
        if (i == (size_t)-1) break;
        if (h.url.length() && h.name.length() && (int)out.size() < limit) {
            out.push_back(h);
        }
        i = skipWs(body, i);
        if (i < body.length() && body[i] == ',') i++;
    }
    return (int)out.size();
}

// Internal helper: generic "names-only" cache populator. Radio-Browser's
// /tags and /countries endpoints return JSON arrays of {"name":"...",
// "stationcount":N}. We only care about name, ordered by stationcount
// descending.
static int fillNamesCache(const String &path,
                          std::vector<String> &cache,
                          int max) {
    if (cache.size() >= (size_t)max) return (int)cache.size();
    String body;
    if (httpGet(path, body) != 200) return 0;

    cache.clear();
    size_t i = 0;
    i = skipWs(body, i);
    if (i >= body.length() || body[i] != '[') return 0;
    i++;
    while (i < body.length() && (int)cache.size() < max) {
        i = skipWs(body, i);
        if (i >= body.length() || body[i] == ']') break;
        if (body[i] != '{') break;
        i++;
        String name;
        while (i < body.length()) {
            i = skipWs(body, i);
            if (i >= body.length() || body[i] == '}') { i++; break; }
            String key;
            i = parseString(body, i, key);
            if (i == (size_t)-1) return 0;
            i = skipWs(body, i);
            if (i >= body.length() || body[i] != ':') return 0;
            i++;
            i = skipWs(body, i);
            if (key == "name") {
                String val;
                i = parseString(body, i, val);
                if (i == (size_t)-1) return 0;
                if (val.length()) name = val;
            } else {
                i = skipValue(body, i);
                if (i == (size_t)-1) return 0;
            }
            i = skipWs(body, i);
            if (i < body.length() && body[i] == ',') i++;
        }
        if (name.length()) cache.push_back(name);
        i = skipWs(body, i);
        if (i < body.length() && body[i] == ',') i++;
    }
    return (int)cache.size();
}

int discoverTopTags(std::vector<String> &out, int max) {
    if (s_tagsCache.empty()) {
        fillNamesCache("/json/tags?order=stationcount&reverse=true&hidebroken=true&limit=60",
                       s_tagsCache, 60);
    }
    out.clear();
    int n = (int)s_tagsCache.size(); if (n > max) n = max;
    for (int i = 0; i < n; i++) out.push_back(s_tagsCache[i]);
    return n;
}

int discoverTopCountries(std::vector<String> &out, int max) {
    if (s_countriesCache.empty()) {
        fillNamesCache("/json/countries?order=stationcount&reverse=true&hidebroken=true",
                       s_countriesCache, 200);
    }
    out.clear();
    int n = (int)s_countriesCache.size(); if (n > max) n = max;
    for (int i = 0; i < n; i++) out.push_back(s_countriesCache[i]);
    return n;
}
