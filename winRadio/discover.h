// Radio-Browser Community API client (https://api.radio-browser.info).
// No auth, no key. Free-form search, genre (tag), country, and ordering
// are all supported server-side; we expose the trimmed set of fields we
// actually render on the /discover web page.
//
// JSON is parsed by hand (Radio-Browser responses are flat arrays of
// objects with known keys) so we avoid adding ArduinoJson to lib_deps
// -- one less thing to break on a fussy internet connection.

#pragma once

#include <Arduino.h>
#include <vector>

struct DiscoverHit {
    String name;
    String url;          // resolved stream endpoint, ready for connecttohost
    String homepage;
    String favicon;
    String tags;         // comma-separated
    String country;      // ISO name, e.g. "United Kingdom"
    String codec;        // "MP3", "AAC", "OGG", ...
    int    bitrate;      // kbps
};

// Build a /json/stations/search URL using the supplied filters. Empty
// strings mean "no filter".
//   query     -> name=<query>  (also limited to server-supplied
//                               name searches)
//   tag       -> tag=<tag>
//   country   -> country=<country>
//   limit     -> limit=<N> (server default 10000, we cap)
int   discoverSearch(const String &query,
                     const String &tag,
                     const String &country,
                     int limit,
                     std::vector<DiscoverHit> &out);

// Return a short cached list of the most common genre / tag names. First
// call populates from the server; subsequent calls use the cache.
int   discoverTopTags(std::vector<String> &out, int max);

// Return the list of country names the server knows about. Cached like
// tags. Intended for the web UI's geography dropdown.
int   discoverTopCountries(std::vector<String> &out, int max);

// Diagnostic.
const char *discoverLastError();
