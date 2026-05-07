#include "web.h"
#include "config.h"
#include "radio_audio.h"
#include "stations.h"
#include "storage.h"
#include "net.h"
#include "display.h"
#include "clock.h"
#include "discover.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <vector>

// Shared ON8CIT visual identity (defined in branding.cpp). Declared at
// global (non-anonymous) scope so the linker binds to the external
// symbols; putting them inside the anonymous namespace below would make
// them internal-linkage-only and the link would fail with "used but
// never defined".
const char *on8citPageCss();
const char *on8citLogoSvg();
const char *on8citFaviconSvg();

namespace {
WebServer s_http(80);
bool      s_running = false;
bool      s_mdnsUp  = false;
uint32_t  s_mdnsLastCheckMs = 0;
// ESPmDNS occasionally stops responding after a router reboot or a lease
// expiry. Re-call MDNS.begin() every 30 s while STA is up. MDNS.begin() is
// idempotent (it tears down the previous service record), so calling it
// unconditionally is cheap.
constexpr uint32_t kMdnsHeartbeatMs = 30000;

String htmlEscape(const String &s) {
    String out; out.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        switch (c) {
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '&':  out += "&amp;";  break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:   out += c;
        }
    }
    return out;
}

// Shared page chrome + banner so every route keeps the ON8CIT branding.
String pageHead(const char *title) {
    String p;
    p.reserve(2600);
    p += F("<!doctype html><html><head><meta charset=utf-8>"
           "<meta name=viewport content='width=device-width,initial-scale=1'>"
           "<link rel=icon type=image/svg+xml href=/favicon.svg>"
           "<title>");
    p += title;
    p += F("</title><style>");
    p += on8citPageCss();
    p += F("</style></head><body>"
           "<header>"
           "<a class=brand href=/ title='Home'>");
    p += on8citLogoSvg();
    p += F("<h1><span class=yel>ON8CIT</span> <span class=cya>WebRadio</span></h1>"
           "</a>"
           // Live banner clock. Server-renders the current time as a
           // seed; client-side JS at the bottom of every page ticks
           // it once a second using the local browser clock.
           "<span class=bannerClock id=bClock>");
    {
        char tbuf[16];
        clockFormatTime(tbuf, sizeof(tbuf));
        p += (tbuf[0] ? String(tbuf) : String("--:--:--"));
    }
    p += F("</span>"
           "<a class=homeBtn href=/ title='Home' aria-label='Home'>&#127968;</a>"
           "</header>");
    return p;
}

String pageFoot() {
    String p;
    p += F("<footer>");
    p += FIRMWARE_NAME; p += F(" "); p += FIRMWARE_VERSION;
    p += F(" &mdash; <a href=/>home</a> &middot; <a href=/discover>discover</a>"
           " &middot; <a href=/wifi>wifi</a>"
           " &middot; <a href=/api-docs>API</a></footer>"
           // Banner clock: parse the server-rendered HH:MM:SS,
           // increment locally every second so the display stays
           // current without a page refresh.
           "<script>(function(){"
           "const el=document.getElementById('bClock');"
           "if(!el)return;"
           "const m=(el.textContent||'').match(/(\\d+):(\\d+):(\\d+)/);"
           "if(!m)return;"
           "let h=+m[1],mi=+m[2],s=+m[3];"
           "setInterval(()=>{"
           " s++;if(s>=60){s=0;mi++;}"
           " if(mi>=60){mi=0;h++;}"
           " if(h>=24)h=0;"
           " el.textContent=String(h).padStart(2,'0')+':'+"
           "  String(mi).padStart(2,'0')+':'+String(s).padStart(2,'0');"
           "},1000);"
           "})();</script>"
           "</body></html>");
    return p;
}

// ---- JSON helpers for the API ------------------------------------------
String jsonEscape(const String &s) {
    String out; out.reserve(s.length() + 4);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((uint8_t)c < 0x20) { char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", c); out += buf; }
                else out += c;
        }
    }
    return out;
}

String stateJson() {
    String j; j.reserve(512);
    j += F("{");
    j += F("\"hostname\":\""); j += jsonEscape(netHostname()); j += F("\",");
    j += F("\"ip\":\"");       j += jsonEscape(netLocalIp()); j += F("\",");
    j += F("\"rssi\":");       j += netRssi();                j += F(",");
    j += F("\"ssid\":\"");     j += jsonEscape(WiFi.SSID());  j += F("\",");
    j += F("\"station\":");    j += audioCurrentStation();    j += F(",");
    j += F("\"stations\":");   j += stationsCount();          j += F(",");
    j += F("\"stationName\":\""); j += jsonEscape(audioStationDisplayName(audioCurrentStation())); j += F("\",");
    j += F("\"icy\":\"");      j += jsonEscape(audioCurStation());  j += F("\",");
    j += F("\"song\":\"");     j += jsonEscape(audioSongPlaying()); j += F("\",");
    j += F("\"bitrate\":");    j += audioBitrate();           j += F(",");
    j += F("\"volume\":");     j += audioVolume();            j += F(",");
    j += F("\"volumeRaw\":");  j += audioVolumeRaw();         j += F(",");
    j += F("\"eqBass\":");     j += audioEqBass();            j += F(",");
    j += F("\"eqMid\":");      j += audioEqMid();             j += F(",");
    j += F("\"eqTreble\":");   j += audioEqTreble();          j += F(",");
    j += F("\"running\":");    j += audioIsRunning() ? "true" : "false"; j += F(",");
    j += F("\"paused\":");     j += audioIsPaused()  ? "true" : "false"; j += F(",");
    {
        char dbuf[32], tbuf[16];
        clockFormatDate(dbuf, sizeof(dbuf));
        clockFormatTime(tbuf, sizeof(tbuf));
        j += F("\"clockSynced\":"); j += clockIsSynced() ? "true" : "false"; j += F(",");
        j += F("\"clockDate\":\""); j += jsonEscape(dbuf); j += F("\",");
        j += F("\"clockTime\":\""); j += jsonEscape(tbuf); j += F("\",");
        j += F("\"timezone\":\"");  j += jsonEscape(clockTimezone()); j += F("\"");
    }
    j += F("}");
    return j;
}

// ---- Routes ---------------------------------------------------------------
void handleIndex() {
    String p = pageHead("ON8CIT WebRadio");
    int cur = audioCurrentStation();
    int n   = stationsCount();
    // (Banner now shows the live clock; the dedicated Clock card is
    // gone. Timezone control moved to the System card modal.)

    // Stations card lives at the very top per user spec -- it's the
    // most-used control on this page. The edit modal HTML lives
    // further down (with the other modals); CSS/JS positions it as
    // a fixed overlay so DOM order doesn't matter.
    p += F("<div class=card><h2>Stations</h2>");
    p += F("<a href=/discover class=bigDiscover onclick=\"showLoading('Opening Discover...')\">"
           "&#128269; Discover more stations</a>");
    p += F("<div id=stationList class=grid>");
    for (int i = 0; i < n; i++) {
        p += F("<div class=stationRow draggable=true data-idx=");
        p += i;
        p += F(">");
        p += F("<span class=dragHandle title='Drag to reorder'>&#9776;</span>");
        p += F("<form method=POST action=/api/station class=stationPick>");
        p += F("<input type=hidden name=n value="); p += i; p += F(">");
        p += F("<button class='station");
        if (i == cur) p += F(" cur");
        p += F("' type=submit><strong>"); p += (i + 1); p += F(".</strong> ");
        p += htmlEscape(audioStationDisplayName(i));
        p += F("<small>"); p += htmlEscape(stationsUrl(i)); p += F("</small>");
        p += F("</button></form>");
        p += F("<button type=button class=infoBtn title='Edit name + URL' "
               "onclick=\"openEdit(");
        p += i; p += F(",");
        p += "'"; p += htmlEscape(stationsName(i)); p += "',";
        p += "'"; p += htmlEscape(stationsUrl(i)); p += "')\">&#9432;</button>";
        p += F("</div>");
    }
    p += F("</div>");
    if (n < stationsMax()) {
        p += F("<p style='margin-top:.6em'>"
               "<button type=button onclick=\"openAdd()\">+ Add station</button>"
               " <span style='color:#8aa;font-size:.85em'>");
        p += n; p += F(" / "); p += stationsMax(); p += F(" slots used</span></p>");
    } else {
        p += F("<p style='color:#8aa;font-size:.85em;margin-top:.6em'>List is full (");
        p += stationsMax(); p += F("). Delete a slot to add more.</p>");
    }
    p += F("</div>");

    p += F("<div class=card><h2>Now playing</h2><div class=kv>");
    p += F("<div>Station</div><div>");
    p += htmlEscape(audioStationDisplayName(cur));
    p += F(" &mdash; slot "); p += (cur + 1); p += F(" / "); p += n;
    p += F("</div>");
    p += F("<div>Song</div><div>");
    {
        const char *sp = audioSongPlaying();
        p += (sp && *sp) ? htmlEscape(sp) : String("&mdash;");
    }
    p += F("</div>");
    p += F("<div>Bitrate</div><div>"); p += audioBitrate(); p += F(" kbps</div>");
    p += F("<div>Volume</div><div>");  p += audioVolume();  p += F(" / 5</div>");
    p += F("<div>WiFi</div><div>");    p += htmlEscape(WiFi.SSID());
    p += F(" ("); p += netRssi(); p += F(" dBm)</div>");
    p += F("</div></div>");

    p += F("<div class=card><h2>Controls</h2><div class=row>"
           "<form method=POST action=/api/prev><button>Prev</button></form>");
    // Explicit Play + Pause buttons (always-visible). Both POST to
    // /api/play (which is a toggle on the audio side); the disabled
    // attribute makes the meaning unambiguous regardless of state.
    bool paused = audioIsPaused();
    p += F("<form method=POST action=/api/play><button");
    if (!paused) p += F(" disabled");
    p += F(">&#9654; Play</button></form>");
    p += F("<form method=POST action=/api/play><button");
    if (paused)  p += F(" disabled");
    p += F(">&#10074;&#10074; Pause</button></form>");
    p += F("<form method=POST action=/api/next><button>Next</button></form>");
    p += F("</div>");

    // Fine volume slider (0..21). Uses POST /api/volume?raw=N.
    p += F("<form method=POST action=/api/volume style='margin-top:.8em'>"
           "<label>Volume (fine, 0&ndash;21): <output name=vo id=vo>");
    p += audioVolumeRaw();
    p += F("</output></label>"
           "<input type=range name=raw min=0 max=21 value=");
    p += audioVolumeRaw();
    p += F(" oninput=\"vo.value=this.value\">"
           "<button type=submit>Apply</button></form>");

    // 3-band EQ. Values -40..+6 dB.
    p += F("<form method=POST action=/api/eq style='margin-top:.8em'>"
           "<label>Bass <output id=eb>"); p += audioEqBass();
    p += F("</output> dB <input type=range name=b min=-40 max=6 value=");
    p += audioEqBass();
    p += F(" oninput=\"eb.value=this.value\"></label>");
    p += F("<label>Mid <output id=em>"); p += audioEqMid();
    p += F("</output> dB <input type=range name=m min=-40 max=6 value=");
    p += audioEqMid();
    p += F(" oninput=\"em.value=this.value\"></label>");
    p += F("<label>Treble <output id=et>"); p += audioEqTreble();
    p += F("</output> dB <input type=range name=t min=-40 max=6 value=");
    p += audioEqTreble();
    p += F(" oninput=\"et.value=this.value\"></label>");
    p += F("<button type=submit>Apply EQ</button></form>");
    p += F("</div>");

    // Modal + script for /api/station-edit and /api/station-del.
    // Same modal serves both "edit existing slot" and "add new slot";
    // Add is triggered by posting n = stationsCount().
    p += F(
      "<div id=modalBg onclick=\"closeEdit(event)\"></div>"
      "<div id=modal>"
      "<h2 id=mtitle>Edit station <span id=mi></span></h2>"
      "<p style='color:#666;margin-top:-.4em;font-size:.9em' id=mhint>"
      "Edit the name and URL for this slot.</p>"
      "<form method=POST action=/api/station-edit>"
      "<input type=hidden name=n id=mn>"
      "<label>Friendly name"
      "<input name=name id=mname maxlength=60 placeholder='(name)'></label>"
      "<label>URL"
      "<input name=url id=murl maxlength=200 placeholder='(url)'></label>"
      "<div class=row style='margin-top:.6em'>"
      "<button type=submit>Save</button>"
      "<button type=button id=mdelBtn class=warn "
      "onclick=\"if(confirm('Delete this station?')){"
      "const f=document.createElement('form');f.method='POST';"
      "f.action='/api/station-del';"
      "const i=document.createElement('input');i.name='n';i.value=mn.value;"
      "f.appendChild(i);document.body.appendChild(f);f.submit();}\">Delete</button>"
      "<button type=button onclick=\"closeEdit()\">Cancel</button>"
      "</div></form></div>"
      "<script>"
      "function openEdit(n,name,url){"
      " mtitle.firstChild.nodeValue='Edit station ';"
      " mi.textContent=n+1;"
      " mn.value=n;"
      " mname.value=name||'';"
      " murl.value=url||'';"
      " mhint.textContent='Edit the name and URL for this slot.';"
      " mdelBtn.style.display='';"
      " modal.classList.add('show');"
      " modalBg.classList.add('show');"
      "}"
      "function openAdd(){"
      " mtitle.firstChild.nodeValue='Add station ';"
      " mi.textContent='';"
      " mn.value=-1;"
      " mname.value='';"
      " murl.value='';"
      " mhint.textContent='Add a new station to the end of the list.';"
      " mdelBtn.style.display='none';"
      " modal.classList.add('show');"
      " modalBg.classList.add('show');"
      "}"
      "function closeEdit(e){"
      " if(e&&e.target&&e.target.id!=='modalBg')return;"
      " modal.classList.remove('show');"
      " modalBg.classList.remove('show');"
      "}"
      "</script>");

    // System card: reach-everything controls at the bottom of the
    // page. Timezone now lives behind the "Set timezone..." button
    // (modal pop-up) so it doesn't clutter the home page; the clock
    // itself ticks in the banner.
    p += F("<div class=card><h2>System</h2>"
           "<p style='margin:.1em 0 .6em'>"
           "<a href=/wifi>Manage WiFi networks</a>"
           "</p>"
           "<div class=row>"
           "<button type=button onclick=\"openTz()\">"
           "&#128344; Set timezone...</button>"
           "<form method=POST action=/api/reboot "
           "onsubmit=\"if(!confirm('Reboot the radio?'))return false;"
           "showLoading('Rebooting...')\">"
           "<button class=warn>&#x21bb; Reboot radio</button></form>"
           "</div></div>");

    // Timezone modal. POSTs to /api/timezone, same endpoint the old
    // Clock-card form used. Preset dropdown fills the free-text
    // POSIX TZ field; both end up sending tz=<string>.
    p += F(
      "<div id=tzModalBg onclick=\"closeTz(event)\"></div>"
      "<div id=tzModal class=modal>"
      "<h2>Set timezone</h2>"
      "<p style='color:#666;margin-top:-.4em;font-size:.9em'>"
      "Pick a preset or paste a POSIX TZ string. The banner clock "
      "and the on-device big-clock both follow this setting.</p>"
      "<form method=POST action=/api/timezone>"
      "<label>Preset"
      "<select name=preset onchange=\"tz.value=this.value\">"
      "<option value=''>-- presets --</option>"
      "<option value='UTC0'>UTC</option>"
      "<option value='GMT0BST,M3.5.0/1,M10.5.0'>Europe/London (UK)</option>"
      "<option value='CET-1CEST,M3.5.0,M10.5.0/3'>Europe/Paris + Berlin</option>"
      "<option value='EST5EDT,M3.2.0,M11.1.0'>US Eastern</option>"
      "<option value='CST6CDT,M3.2.0,M11.1.0'>US Central</option>"
      "<option value='MST7MDT,M3.2.0,M11.1.0'>US Mountain</option>"
      "<option value='PST8PDT,M3.2.0,M11.1.0'>US Pacific</option>"
      "<option value='JST-9'>Asia/Tokyo</option>"
      "<option value='AEST-10AEDT,M10.1.0,M4.1.0/3'>Australia/Sydney</option>"
      "</select></label>"
      "<label>POSIX TZ string"
      "<input name=tz id=tz value=\"");
    p += htmlEscape(clockTimezone());
    p += F("\" required></label>"
      "<div class=row style='margin-top:.6em'>"
      "<button type=submit>Save timezone</button>"
      "<button type=button onclick=\"closeTz()\">Cancel</button>"
      "</div></form></div>"
      "<script>"
      "function openTz(){"
      " tzModal.classList.add('show');"
      " tzModalBg.classList.add('show');"
      "}"
      "function closeTz(e){"
      " if(e&&e.target&&e.target.id!=='tzModalBg')return;"
      " tzModal.classList.remove('show');"
      " tzModalBg.classList.remove('show');"
      "}"
      "</script>");

    // Loading modal + nav-click helper (shared with /discover).
    p += F(
      "<div id=loading style='display:none;position:fixed;inset:0;"
      "background:rgba(0,0,0,.65);align-items:center;justify-content:center;"
      "z-index:50'>"
      "<div style='background:#141720;border:1px solid #2a2f3c;border-radius:8px;"
      "padding:1.2em 1.6em;color:#e6e7ea;box-shadow:0 8px 32px rgba(0,0,0,.6)'>"
      "<span id=loadingText>Loading...</span></div></div>"
      "<script>"
      "const LM=document.getElementById('loading');"
      "const LT=document.getElementById('loadingText');"
      "function showLoading(t){LT.textContent=t||'Loading...';LM.style.display='flex'}"
      "window.addEventListener('pageshow',()=>LM.style.display='none');"
      "document.querySelectorAll('form').forEach(f=>{"
      " f.addEventListener('submit',()=>showLoading('Working...'));"
      "});"
      // Same link-click feedback as /discover, so first-time nav to the
      // Discover page (slow TLS + API round trip, no browser cache)
      // doesn't look frozen.
      "document.querySelectorAll('a[href]').forEach(a=>{"
      " const h=a.getAttribute('href');"
      " if(!h||h.startsWith('#')||h.startsWith('http')||a.target==='_blank')return;"
      " a.addEventListener('click',()=>showLoading('Loading page...'));"
      "});"
      // ---- Scroll preservation across station edits / adds / deletes.
      // The relevant POSTs 302 back to / on success; we save the current
      // scrollY before submit and restore it on the next pageshow.
      "function saveScroll(){sessionStorage.setItem('homeScroll',window.scrollY||0);}"
      "document.querySelectorAll("
      " 'form[action=\"/api/station-edit\"],"
      " form[action=\"/api/station-del\"],"
      " form[action=\"/api/station\"]')"
      ".forEach(f=>f.addEventListener('submit',saveScroll));"
      "window.addEventListener('pageshow',()=>{"
      " const y=sessionStorage.getItem('homeScroll');"
      " if(y!==null){window.scrollTo(0,parseInt(y,10));sessionStorage.removeItem('homeScroll');}"
      "});"
      // ---- Drag-and-drop reorder. Uses HTML5 native DnD (works on any
      // desktop browser; mobile users are best served by editing the
      // list explicitly via the modal). On drop we POST to
      // /api/station-move and reload to pick up the canonical order +
      // the now-correct slot numbers.
      "(function(){"
      "const list=document.getElementById('stationList');"
      "if(!list)return;"
      "let dragSrc=null;"
      "list.querySelectorAll('.stationRow').forEach(row=>{"
      " row.addEventListener('dragstart',e=>{"
      "  dragSrc=row;row.classList.add('dragging');"
      "  e.dataTransfer.effectAllowed='move';"
      "  e.dataTransfer.setData('text/plain',row.dataset.idx||'');"
      " });"
      " row.addEventListener('dragend',()=>{row.classList.remove('dragging');"
      "  list.querySelectorAll('.dropTarget').forEach(r=>r.classList.remove('dropTarget'));});"
      " row.addEventListener('dragover',e=>{"
      "  if(!dragSrc||dragSrc===row)return;"
      "  e.preventDefault();e.dataTransfer.dropEffect='move';"
      "  list.querySelectorAll('.dropTarget').forEach(r=>r.classList.remove('dropTarget'));"
      "  row.classList.add('dropTarget');"
      " });"
      " row.addEventListener('drop',async e=>{"
      "  e.preventDefault();"
      "  if(!dragSrc||dragSrc===row)return;"
      "  const from=parseInt(dragSrc.dataset.idx,10);"
      "  const to=parseInt(row.dataset.idx,10);"
      "  saveScroll();"
      "  showLoading('Reordering...');"
      "  const fd=new FormData();fd.append('from',from);fd.append('to',to);"
      "  try{"
      "   const r=await fetch('/api/station-move',{method:'POST',body:fd});"
      "   if(!r.ok)throw new Error('HTTP '+r.status);"
      "   location.href='/';"
      "  }catch(err){alert('Reorder failed: '+err.message);location.reload();}"
      " });"
      "});"
      "})();"
      "</script>");

    p += pageFoot();
    s_http.send(200, "text/html", p);
}

void handleState() {
    s_http.send(200, "application/json", stateJson());
}

void handleStation() {
    if (!s_http.hasArg("n")) { s_http.send(400, "text/plain", "missing n"); return; }
    int n = s_http.arg("n").toInt();
    if (n < 0 || n >= stationsCount()) { s_http.send(400, "text/plain", "out of range"); return; }
    audioSelectStation(n);
    s_http.sendHeader("Location", "/");
    s_http.send(302);
}

// POST /api/station-edit
// n = 0..count-1         : edit in place
// n = count or n = -1    : append (Add)
// The AJAX save on /discover uses n = count and fresh (name,url) to
// append a discovered station; the home-page modal uses the same path
// with n set to the specific slot.
void handleStationEdit() {
    if (!s_http.hasArg("n"))   { s_http.send(400, "text/plain", "missing n"); return; }
    if (!s_http.hasArg("url")) { s_http.send(400, "text/plain", "missing url"); return; }
    int n       = s_http.arg("n").toInt();
    String name = s_http.arg("name"); name.trim();
    String url  = s_http.arg("url");  url.trim();
    if (url.length() == 0) { s_http.send(400, "text/plain", "empty url"); return; }

    if (n < 0 || n >= stationsCount()) {
        // Add new entry.
        if (stationsCount() >= stationsMax()) {
            s_http.send(409, "text/plain", "station list full");
            return;
        }
        int idx = stationsAdd(name, url);
        if (idx < 0) { s_http.send(500, "text/plain", "save failed"); return; }
    } else {
        if (!stationsEdit(n, name, url)) {
            s_http.send(500, "text/plain", "save failed");
            return;
        }
    }
    s_http.sendHeader("Location", "/");
    s_http.send(302);
}

// POST /api/station-move  (from = source idx, to = destination idx)
// Used by the drag-drop reorder UI on the home page.
void handleStationMove() {
    if (!s_http.hasArg("from") || !s_http.hasArg("to")) {
        s_http.send(400, "text/plain", "missing from/to"); return;
    }
    int from = s_http.arg("from").toInt();
    int to   = s_http.arg("to").toInt();
    int curBefore = audioCurrentStation();
    if (!stationsMove(from, to)) {
        s_http.send(400, "text/plain", "out of range"); return;
    }
    // Track the playing slot through the move so the audio pointer
    // keeps pointing at the same stream rather than at whatever
    // station happens to occupy the old index.
    int curAfter = curBefore;
    if (curBefore == from) {
        curAfter = to;
    } else if (from < to) {
        if (curBefore > from && curBefore <= to) curAfter = curBefore - 1;
    } else {
        if (curBefore < from && curBefore >= to) curAfter = curBefore + 1;
    }
    if (curAfter != curBefore) audioSetCurrentSlot(curAfter);
    s_http.send(200, "text/plain", "ok");
}

// POST /api/station-del  (n = slot index)
void handleStationDel() {
    if (!s_http.hasArg("n")) { s_http.send(400, "text/plain", "missing n"); return; }
    int n = s_http.arg("n").toInt();
    if (n < 0 || n >= stationsCount()) { s_http.send(400, "text/plain", "out of range"); return; }
    int wasCurrent = audioCurrentStation();
    if (!stationsDelete(n)) { s_http.send(500, "text/plain", "delete failed"); return; }
    // If we deleted the currently-playing slot, fall back to slot 0
    // (or stop audio if the list is now empty).
    if (wasCurrent == n && stationsCount() > 0) audioSelectStation(0);
    s_http.sendHeader("Location", "/");
    s_http.send(302);
}

void handleNext()  { audioNextStation(); s_http.sendHeader("Location", "/"); s_http.send(302); }
void handlePrev()  { audioPrevStation(); s_http.sendHeader("Location", "/"); s_http.send(302); }
void handlePlay()  { audioTogglePause(); s_http.sendHeader("Location", "/"); s_http.send(302); }

void handleVolume() {
    if (s_http.hasArg("raw")) {
        audioSetVolumeRaw(s_http.arg("raw").toInt());
    } else if (s_http.hasArg("v")) {
        audioSetVolume(s_http.arg("v").toInt());
    } else if (s_http.hasArg("d")) {
        int delta = s_http.arg("d").toInt();
        audioSetVolume(audioVolume() + delta);
    }
    s_http.sendHeader("Location", "/");
    s_http.send(302);
}

void handleEq() {
    int8_t b = audioEqBass();
    int8_t m = audioEqMid();
    int8_t t = audioEqTreble();
    if (s_http.hasArg("b")) b = (int8_t)s_http.arg("b").toInt();
    if (s_http.hasArg("m")) m = (int8_t)s_http.arg("m").toInt();
    if (s_http.hasArg("t")) t = (int8_t)s_http.arg("t").toInt();
    audioSetEq(b, m, t);
    s_http.sendHeader("Location", "/");
    s_http.send(302);
}

bool s_rebootPending = false;
uint32_t s_rebootAtMs = 0;

void handleReboot() {
    String p = pageHead("Rebooting");
    p += F("<div class=card><h2>Rebooting...</h2><p>The radio is restarting. "
           "This page will be unreachable for a few seconds.</p></div>");
    p += pageFoot();
    s_http.send(200, "text/html", p);
    s_rebootPending = true;
    s_rebootAtMs    = millis() + 1500;
}

void handleTimezone() {
    if (!s_http.hasArg("tz")) { s_http.send(400, "text/plain", "missing tz"); return; }
    String tz = s_http.arg("tz"); tz.trim();
    if (!clockSetTimezone(tz.c_str())) { s_http.send(400, "text/plain", "empty tz"); return; }
    s_http.sendHeader("Location", "/");
    s_http.send(302);
}

void handleFavicon() {
    s_http.sendHeader("Cache-Control", "max-age=86400");
    s_http.send(200, "image/svg+xml", on8citFaviconSvg());
}

void handleApiDocs() {
    String p = pageHead("ON8CIT WebRadio -- API");
    p += F("<div class=card><h2>HTTP API</h2>"
           "<p style='color:#8aa;font-size:.9em'>Every control on the home "
           "page and on <a href=/discover>Discover</a> is wired through "
           "these endpoints. Use them for your own automation.</p>"
           "<div class=kv>"
           "<div>GET /api/state</div>"
           "<div>Full state as JSON (ssid, station, song, volume, paused, clock ...)</div>"
           "<div>POST /api/station</div><div>Body: n=&lt;0..N-1&gt; -- select saved slot</div>"
           "<div>POST /api/station-edit</div>"
           "<div>Body: n=&lt;slot&gt; name=... url=...; n=-1 or n=count appends</div>"
           "<div>POST /api/station-del</div>"
           "<div>Body: n=&lt;slot&gt; -- remove (list shifts down)</div>"
           "<div>POST /api/station-move</div>"
           "<div>Body: from=&lt;src&gt; to=&lt;dst&gt; -- reorder</div>"
           "<div>POST /api/next</div><div>Cycle to the next saved slot</div>"
           "<div>POST /api/prev</div><div>Cycle to the previous saved slot</div>"
           "<div>POST /api/play</div><div>Toggle pause / resume</div>"
           "<div>POST /api/volume</div>"
           "<div>Body: raw=&lt;0..21&gt; or v=&lt;1..5&gt; or d=&pm;1</div>"
           "<div>POST /api/eq</div>"
           "<div>Body: b=&lt;-40..6&gt; m=&lt;-40..6&gt; t=&lt;-40..6&gt;</div>"
           "<div>POST /api/listen</div>"
           "<div>Body: url=... name=... -- ad-hoc preview play (Discover uses this)</div>"
           "<div>POST /api/timezone</div><div>Body: tz=&lt;POSIX string&gt;</div>"
           "<div>POST /api/wifi-connect</div>"
           "<div>Body: idx=&lt;saved index&gt; -- switch to a saved network</div>"
           "<div>POST /api/wifi-connect-adhoc</div>"
           "<div>Body: ssid=... pass=... -- ad-hoc connect, save on success</div>"
           "<div>POST /api/wifi-add</div>"
           "<div>Body: ssid=... pass=... -- save creds without connecting</div>"
           "<div>POST /api/wifi-del</div>"
           "<div>Body: idx=&lt;n&gt; -- remove a saved network</div>"
           "<div>POST /api/wifi-move</div>"
           "<div>Body: from=&lt;n&gt; to=&lt;n&gt; -- reorder saved list</div>"
           "<div>POST /api/wifi-rescan</div>"
           "<div>Triggers a fresh visible-network scan</div>"
           "<div>POST /api/reboot</div><div>Restarts the radio</div>"
           "</div>"
           "</div>"
           "<p><a href=/>&laquo; Home</a> &middot; <a href=/discover>Discover</a></p>");
    p += pageFoot();
    s_http.send(200, "text/html", p);
}

// ---- /discover --------------------------------------------------------
// Source selector (Radio-Browser for now; the UI is designed so more
// sources can slot in as new enum values + another search function).
enum DiscoverSource { SRC_RADIOBROWSER = 0 };
static const char *sourceName(int s) {
    switch (s) {
        case SRC_RADIOBROWSER: return "Radio-Browser";
        default:               return "Unknown";
    }
}

// Trim "tags" to the first N entries so the UI doesn't end up rendering
// a comma-separated wall of 30 tags per station.
static String truncTags(const String &tags, int maxTags) {
    String out;
    int count = 0, start = 0;
    while (count < maxTags && start < (int)tags.length()) {
        int comma = tags.indexOf(',', start);
        String tok = (comma < 0) ? tags.substring(start) : tags.substring(start, comma);
        tok.trim();
        if (tok.length()) {
            if (out.length()) out += ", ";
            out += tok;
            count++;
        }
        if (comma < 0) break;
        start = comma + 1;
    }
    return out;
}

void handleDiscover() {
    String q       = s_http.arg("q");
    String tag     = s_http.arg("tag");
    String country = s_http.arg("country");
    int    src     = s_http.hasArg("src") ? s_http.arg("src").toInt() : SRC_RADIOBROWSER;

    std::vector<DiscoverHit>    hits;
    std::vector<String>         tags, countries;
    bool didSearch = q.length() || tag.length() || country.length();
    int  found     = 0;
    if (didSearch && src == SRC_RADIOBROWSER) {
        found = discoverSearch(q, tag, country, 40, hits);
    }
    discoverTopTags(tags, 40);           // alphabetical
    discoverTopCountries(countries, 240); // alphabetical

    String p = pageHead("ON8CIT WebRadio -- Discover");

    // Search form.
    p += F("<div class=card><h2>Discover</h2>"
           "<form id=searchForm method=GET action=/discover "
           "style='display:flex;flex-direction:column;gap:.25em'>"
           "<label>Source <select name=src>");
    for (int s = 0; s <= SRC_RADIOBROWSER; s++) {
        p += F("<option value=");
        p += s;
        if (s == src) p += F(" selected");
        p += F(">");
        p += sourceName(s);
        p += F("</option>");
    }
    // Stub entries for the planned additional sources so the user can
    // see the roadmap. Disabled until their respective client lands.
    p += F("<option disabled>SHOUTcast YP (coming soon)</option>");
    p += F("<option disabled>TuneIn OPML (coming soon)</option>");
    p += F("</select></label>");

    p += F("<label>Search (name contains) "
           "<input name=q value='");
    p += htmlEscape(q);
    p += F("'></label>");

    p += F("<label>Genre / tag "
           "<select name=tag><option value=''>-- any --</option>");
    for (auto &t : tags) {
        p += F("<option value='"); p += htmlEscape(t); p += F("'");
        if (t == tag) p += F(" selected");
        p += F(">"); p += htmlEscape(t); p += F("</option>");
    }
    p += F("</select></label>");

    p += F("<label>Country "
           "<select name=country><option value=''>-- any --</option>");
    for (auto &c : countries) {
        p += F("<option value='"); p += htmlEscape(c); p += F("'");
        if (c == country) p += F(" selected");
        p += F(">"); p += htmlEscape(c); p += F("</option>");
    }
    p += F("</select></label>");

    p += F("<button type=submit>Search</button></form></div>");

    if (didSearch) {
        p += F("<div class=card><h2>Results ("); p += found; p += F(")</h2>");
        if (found == 0) {
            p += F("<p style='color:#8aa'>No hits.</p>");
            p += F("<p style='color:#8aa;font-size:.85em'>Last error: ");
            p += htmlEscape(discoverLastError());
            p += F("</p>");
        } else {
            p += F("<ul style='list-style:none;padding:0;margin:0'>");
            int nst = stationsCount();
            int rowIdx = 0;
            for (auto &h : hits) {
                p += F("<li class=stationRow style='flex-direction:column;align-items:stretch;"
                       "gap:.3em;margin:.35em 0;padding:.5em;border:1px solid #242832;"
                       "border-radius:6px;background:#181b24'>");
                // Row 1: name + codec / bitrate / country
                p += F("<div style='display:flex;justify-content:space-between;gap:.5em'>");
                p += F("<strong>"); p += htmlEscape(h.name); p += F("</strong>");
                p += F("<span style='color:#8aa;font-size:.85em'>");
                if (h.bitrate > 0) { p += h.bitrate; p += F(" kbps "); }
                p += htmlEscape(h.codec);
                if (h.country.length()) { p += F(" &middot; "); p += htmlEscape(h.country); }
                p += F("</span></div>");

                // Row 2: tags (first 4), homepage link if any.
                p += F("<div style='color:#8aa;font-size:.85em;display:flex;"
                       "justify-content:space-between;gap:.5em'>");
                p += F("<span>");
                String t4 = truncTags(h.tags, 4);
                p += (t4.length() ? htmlEscape(t4) : String("&mdash;"));
                p += F("</span>");
                if (h.homepage.length()) {
                    p += F("<a href='"); p += htmlEscape(h.homepage);
                    p += F("' target=_blank rel=noopener>Homepage &rarr;</a>");
                }
                p += F("</div>");

                // Row 3: stream URL
                p += F("<small style='color:#556;word-break:break-all;font-size:.8em'>");
                p += htmlEscape(h.url);
                p += F("</small>");

                // Row 4: Listen (AJAX) + Save to slot.
                p += F("<div class=row>");
                p += F("<button class=listenBtn data-url='");
                p += htmlEscape(h.url);
                p += F("' data-name='");
                p += htmlEscape(h.name);
                p += F("'>&#9654; Listen</button>");

                // Save is AJAX so the results page stays put. Default
                // action is Add (append to the list). A secondary
                // <select> lets the user target an existing slot for
                // replacement, for explicit overwrite cases.
                p += F("<form class=saveForm data-name='");
                p += htmlEscape(h.name); p += F("' data-url='");
                p += htmlEscape(h.url);
                p += F("' style='display:flex;gap:.3em;flex:1'>");
                p += F("<select class=slotPick style='flex:1'>");
                if (nst < stationsMax()) {
                    p += F("<option value=-1 selected>+ Add to list</option>");
                } else {
                    p += F("<option value=-1 disabled selected>List full &mdash; replace?</option>");
                }
                for (int s = 0; s < nst; s++) {
                    p += F("<option value="); p += s; p += F(">Replace slot ");
                    p += (s + 1); p += F(": ");
                    p += htmlEscape(audioStationDisplayName(s));
                    p += F("</option>");
                }
                p += F("</select>");
                p += F("<button type=submit>Save</button></form>");
                p += F("</div></li>");
                rowIdx++;
            }
            p += F("</ul>");
        }
        p += F("</div>");
    }

    // Loading modal + AJAX Listen. Kept inline so there's no extra HTTP
    // round-trip for a static asset. <dialog> would be cleaner but
    // isn't supported by every phone browser yet, so we roll our own.
    p += F(
      "<div id=loading style='display:none;position:fixed;inset:0;"
      "background:rgba(0,0,0,.65);align-items:center;justify-content:center;"
      "z-index:50'>"
      "<div style='background:#141720;border:1px solid #2a2f3c;border-radius:8px;"
      "padding:1.2em 1.6em;color:#e6e7ea;font-size:1em;box-shadow:0 8px 32px rgba(0,0,0,.6)'>"
      "<span id=loadingText>Loading...</span></div></div>");

    p += F("<p style='margin-top:1em'><a href=/>&laquo; Home</a></p>");

    // Scripts: loading-modal helper + Listen AJAX + form-submit hook.
    p += F(
      "<script>"
      "const LM=document.getElementById('loading');"
      "const LT=document.getElementById('loadingText');"
      "function showLoading(t){LT.textContent=t||'Loading...';LM.style.display='flex'}"
      "function hideLoading(){LM.style.display='none'}"
      // Search form triggers a loading overlay on submit.
      "const sf=document.getElementById('searchForm');"
      "if(sf)sf.addEventListener('submit',()=>showLoading('Searching stations...'));"
      // Listen buttons: AJAX so we stay on the results page.
      "document.querySelectorAll('.listenBtn').forEach(btn=>{"
      " btn.addEventListener('click',async ()=>{"
      "  const url=btn.dataset.url, name=btn.dataset.name;"
      "  const oldLabel=btn.textContent, allBtns=document.querySelectorAll('.listenBtn');"
      "  allBtns.forEach(b=>b.disabled=true);"
      "  btn.textContent='\\u231B Connecting...';"
      "  showLoading('Asking radio to play...');"
      "  try{"
      "   const fd=new FormData();fd.append('url',url);fd.append('name',name);"
      "   const r=await fetch('/api/listen',{method:'POST',body:fd});"
      "   if(!r.ok)throw new Error('HTTP '+r.status);"
      "   btn.textContent='\\u266A Playing';"
      "   setTimeout(()=>{btn.textContent='\\u25B6 Listen';allBtns.forEach(b=>b.disabled=false);hideLoading();},1500);"
      "  }catch(e){"
      "   btn.textContent='Error';"
      "   setTimeout(()=>{btn.textContent=oldLabel;allBtns.forEach(b=>b.disabled=false);hideLoading();},1500);"
      "  }"
      " });"
      "});"
      // Links into this page from other pages: wake the loading modal
      // so the user sees feedback even if the new page takes a moment
      // to hit the server. Hooked up on DOMContentLoaded of those pages
      // (see the home-page script).
      "window.addEventListener('pageshow',()=>hideLoading());"
      // AJAX save: POST to /api/station-edit, show a brief toast on the
      // same button, stay on the results page.
      "document.querySelectorAll('.saveForm').forEach(f=>{"
      " f.addEventListener('submit',async e=>{"
      "  e.preventDefault();"
      "  const btn=f.querySelector('button');"
      "  const oldLabel=btn.textContent;"
      "  btn.disabled=true; btn.textContent='Saving...';"
      "  try{"
      "   const fd=new FormData();"
      "   fd.append('name',f.dataset.name);"
      "   fd.append('url',f.dataset.url);"
      "   fd.append('n',f.querySelector('.slotPick').value);"
      "   const r=await fetch('/api/station-edit',{method:'POST',body:fd});"
      "   btn.textContent=r.ok?'\\u2713 Saved':'Save failed';"
      "  }catch(err){btn.textContent='Error';}"
      "  setTimeout(()=>{btn.textContent=oldLabel;btn.disabled=false;},1800);"
      " });"
      "});"
      // Show loading modal on any internal link click so first-time
      // Discover navigation (no browser cache yet, slow TLS + API round
      // trip) gives visible feedback. Only triggers for same-origin
      // hrefs that aren't "#..." anchors or external.
      "document.querySelectorAll('a[href]').forEach(a=>{"
      " const h=a.getAttribute('href');"
      " if(!h||h.startsWith('#')||h.startsWith('http')||a.target==='_blank')return;"
      " a.addEventListener('click',()=>showLoading('Loading page...'));"
      "});"
      "</script>");
    p += pageFoot();
    s_http.send(200, "text/html", p);
}

// /api/station-edit retains its existing 302-to-/ behaviour for the
// home-page modal form. The Discover page's Save uses AJAX (above)
// which ignores the redirect and stays on the page. Both paths share
// the same handler.
void handleListen() {
    if (!s_http.hasArg("url")) { s_http.send(400, "text/plain", "missing url"); return; }
    String url  = s_http.arg("url");
    String name = s_http.arg("name");
    bool ok = audioPlayAdhoc(url.c_str(), name.c_str());
    s_http.send(ok ? 200 : 502, "text/plain", ok ? "ok" : "playback failed");
}

// ---- /wifi ----------------------------------------------------------------
// Manages the saved-network list (the same list the boot-time
// connect loop walks). Also surfaces currently-visible APs from the
// last scan so the user can save / connect to one without typing.
//
// We deliberately DO NOT auto-save scanned networks -- the saved list
// is for credentials we know work. wifiAddNetwork is only called
// after a successful connect or an explicit user save.
void handleWifi() {
    String p = pageHead("ON8CIT WebRadio -- WiFi");

    int saved = wifiNetworkCount();
    int curIdx = netLastJoinedSlot();

    // ---- Saved networks card.
    p += F("<div class=card><h2>Saved networks</h2>"
           "<p style='color:#8aa;font-size:.85em;margin:.1em 0 .6em'>"
           "Drag the &#9776; handle to reorder -- the boot-time connect "
           "loop walks this list top-to-bottom.</p>");
    if (saved == 0) {
        p += F("<p style='color:#8aa'>No saved networks. Add one below or "
               "connect to a visible network.</p>");
    } else {
        p += F("<div id=savedList class=grid>");
        for (int i = 0; i < saved; i++) {
            p += F("<div class=stationRow draggable=true data-idx=");
            p += i; p += F(">");
            p += F("<span class=dragHandle title='Drag to reorder'>&#9776;</span>");
            p += F("<div class='station");
            if (i == curIdx) p += F(" cur");
            p += F("' style='flex:1;cursor:default'>");
            p += F("<strong>"); p += (i + 1); p += F(".</strong> ");
            p += htmlEscape(wifiNetworkSsid(i));
            const char *pass = wifiNetworkPass(i);
            if (i == curIdx) p += F("  <small style='color:#FFD400'>(joined)</small>");
            if (strlen(pass) == 0) p += F("  <small>(open)</small>");
            else                    p += F("  <small>(saved password)</small>");
            p += F("</div>");
            p += F("<form method=POST action=/api/wifi-connect class=wifiAct "
                   "title='Connect now'>"
                   "<input type=hidden name=idx value=");
            p += i; p += F(">");
            p += F("<button type=submit>Connect</button></form>");
            p += F("<form method=POST action=/api/wifi-del class=wifiAct "
                   "title='Delete' onsubmit=\"return confirm('Delete this saved network?')\">"
                   "<input type=hidden name=idx value=");
            p += i; p += F(">");
            p += F("<button class=warn type=submit>&#10005;</button></form>");
            p += F("</div>");
        }
        p += F("</div>");
    }
    p += F("</div>");

    // ---- Visible networks card. Only shows scanned SSIDs that aren't
    // already in the saved list, so the two columns don't duplicate.
    int scanCount = netScanCount();
    p += F("<div class=card><h2>Visible networks</h2>"
           "<p style='color:#8aa;font-size:.85em;margin:.1em 0 .6em'>"
           "Last scan result. ");
    p += F("<form method=POST action=/api/wifi-rescan style='display:inline'>"
           "<button type=submit class=infoBtn title='Rescan'>&#x21bb; Rescan</button>"
           "</form></p>");
    if (scanCount == 0) {
        p += F("<p style='color:#8aa'>(no scan results yet)</p>");
    } else {
        // Build a quick set of saved SSIDs.
        p += F("<div class=grid>");
        int shown = 0;
        for (int i = 0; i < scanCount; i++) {
            const ScanResult *r = netScanResult(i);
            if (!r || r->ssid.length() == 0) continue;
            // Hide already-saved SSIDs.
            bool inSaved = false;
            for (int j = 0; j < saved; j++) {
                if (r->ssid.equalsIgnoreCase(wifiNetworkSsid(j))) { inSaved = true; break; }
            }
            if (inSaved) continue;
            shown++;
            p += F("<div class=stationRow>");
            p += F("<div class=station style='flex:1;cursor:default'>");
            p += htmlEscape(r->ssid);
            p += F("  <small>"); p += r->rssi; p += F(" dBm</small>");
            if (r->encryption == WIFI_AUTH_OPEN) p += F("  <small>(open)</small>");
            p += F("</div>");
            p += F("<button type=button class=infoBtn "
                   "onclick=\"openConnectAdhoc('");
            p += htmlEscape(r->ssid);
            p += F("',"); p += (r->encryption == WIFI_AUTH_OPEN ? "true" : "false");
            p += F(")\">Connect</button>");
            p += F("</div>");
        }
        if (shown == 0) {
            p += F("<p style='color:#8aa'>(every visible network is already saved)</p>");
        }
        p += F("</div>");
    }
    p += F("</div>");

    // ---- Add manually card.
    p += F("<div class=card><h2>Add manually</h2>"
           "<p style='color:#8aa;font-size:.85em;margin:.1em 0 .6em'>"
           "Useful for hidden SSIDs. The credentials are saved without "
           "an immediate connect attempt.</p>"
           "<form method=POST action=/api/wifi-add>"
           "<label>SSID <input name=ssid maxlength=32 required></label>"
           "<label>Password <input name=pass type=password maxlength=64></label>"
           "<button type=submit style='margin-top:.5em'>Save</button>"
           "</form></div>");

    // ---- Modal for ad-hoc connect (visible-network connect prompt).
    p += F(
      "<div id=modalBg onclick=\"closeAdhoc(event)\"></div>"
      "<div id=modal>"
      "<h2>Connect to <span id=adSsid></span></h2>"
      "<form method=POST action=/api/wifi-connect-adhoc>"
      "<input type=hidden name=ssid id=adSsidF>"
      "<label>Password <input name=pass id=adPass type=password "
      "placeholder='leave blank if open'></label>"
      "<div class=row style='margin-top:.6em'>"
      "<button type=submit>Connect</button>"
      "<button type=button onclick=\"closeAdhoc()\">Cancel</button>"
      "</div></form></div>");

    p += F(
      "<div id=loading style='display:none;position:fixed;inset:0;"
      "background:rgba(0,0,0,.65);align-items:center;justify-content:center;"
      "z-index:50'>"
      "<div style='background:#141720;border:1px solid #2a2f3c;border-radius:8px;"
      "padding:1.2em 1.6em;color:#e6e7ea;box-shadow:0 8px 32px rgba(0,0,0,.6)'>"
      "<span id=loadingText>Working...</span></div></div>");

    p += F(
      "<script>"
      "const LM=document.getElementById('loading');"
      "const LT=document.getElementById('loadingText');"
      "function showLoading(t){LT.textContent=t||'Working...';LM.style.display='flex'}"
      "window.addEventListener('pageshow',()=>LM.style.display='none');"
      "function openConnectAdhoc(ssid,isOpen){"
      " adSsid.textContent=ssid;adSsidF.value=ssid;adPass.value='';"
      " adPass.placeholder=isOpen?'leave blank for open':'password';"
      " modal.classList.add('show');modalBg.classList.add('show');"
      " setTimeout(()=>adPass.focus(),50);"
      "}"
      "function closeAdhoc(e){"
      " if(e&&e.target&&e.target.id!=='modalBg')return;"
      " modal.classList.remove('show');modalBg.classList.remove('show');"
      "}"
      // Reorder via drag-drop on saved list.
      "(function(){"
      "const list=document.getElementById('savedList');"
      "if(!list)return;"
      "let dragSrc=null;"
      "list.querySelectorAll('.stationRow').forEach(row=>{"
      " row.addEventListener('dragstart',e=>{"
      "  dragSrc=row;row.classList.add('dragging');"
      "  e.dataTransfer.effectAllowed='move';"
      " });"
      " row.addEventListener('dragend',()=>{row.classList.remove('dragging');"
      "  list.querySelectorAll('.dropTarget').forEach(r=>r.classList.remove('dropTarget'));});"
      " row.addEventListener('dragover',e=>{"
      "  if(!dragSrc||dragSrc===row)return;"
      "  e.preventDefault();e.dataTransfer.dropEffect='move';"
      "  list.querySelectorAll('.dropTarget').forEach(r=>r.classList.remove('dropTarget'));"
      "  row.classList.add('dropTarget');"
      " });"
      " row.addEventListener('drop',async e=>{"
      "  e.preventDefault();"
      "  if(!dragSrc||dragSrc===row)return;"
      "  const from=parseInt(dragSrc.dataset.idx,10);"
      "  const to=parseInt(row.dataset.idx,10);"
      "  showLoading('Reordering...');"
      "  const fd=new FormData();fd.append('from',from);fd.append('to',to);"
      "  try{"
      "   const r=await fetch('/api/wifi-move',{method:'POST',body:fd});"
      "   if(!r.ok)throw new Error('HTTP '+r.status);"
      "   location.href='/wifi';"
      "  }catch(err){alert('Reorder failed: '+err.message);location.reload();}"
      " });"
      "});"
      "})();"
      "document.querySelectorAll('form').forEach(f=>f.addEventListener('submit',()=>{"
      " if(f.action.endsWith('/api/wifi-connect')||"
      "    f.action.endsWith('/api/wifi-connect-adhoc'))"
      "  showLoading('Connecting (may interrupt audio for ~10 s)...');"
      " else"
      "  showLoading('Saving...');"
      "}));"
      "</script>");

    p += pageFoot();
    s_http.send(200, "text/html", p);
}

// POST /api/wifi-connect (idx=<saved index>) -- switch to that network.
void handleWifiConnect() {
    if (!s_http.hasArg("idx")) { s_http.send(400, "text/plain", "missing idx"); return; }
    int idx = s_http.arg("idx").toInt();
    if (idx < 0 || idx >= wifiNetworkCount()) {
        s_http.send(400, "text/plain", "out of range"); return;
    }
    String ssid = wifiNetworkSsid(idx);
    String pass = wifiNetworkPass(idx);
    bool ok = netConnectAdhoc(ssid, pass, 12000);
    if (ok) {
        wifiPromoteNetwork(idx);   // user-chosen -> top of boot list
        audioStartLast();
    }
    s_http.sendHeader("Location", "/wifi");
    s_http.send(302);
}

// POST /api/wifi-connect-adhoc (ssid + pass) -- ad-hoc connect, save on success.
void handleWifiConnectAdhoc() {
    if (!s_http.hasArg("ssid")) { s_http.send(400, "text/plain", "missing ssid"); return; }
    String ssid = s_http.arg("ssid"); ssid.trim();
    String pass = s_http.arg("pass");
    if (ssid.length() == 0) { s_http.send(400, "text/plain", "empty ssid"); return; }
    bool ok = netConnectAdhoc(ssid, pass, 12000);
    if (ok) {
        wifiAddNetwork(ssid, pass);
        // Promote: just-connected SSID should win on next boot.
        for (int i = 0; i < wifiNetworkCount(); i++) {
            if (ssid.equalsIgnoreCase(wifiNetworkSsid(i))) {
                wifiPromoteNetwork(i); break;
            }
        }
        audioStartLast();
    }
    s_http.sendHeader("Location", "/wifi");
    s_http.send(302);
}

// POST /api/wifi-add (ssid + pass) -- save creds, no connect attempt.
void handleWifiAdd() {
    if (!s_http.hasArg("ssid")) { s_http.send(400, "text/plain", "missing ssid"); return; }
    String ssid = s_http.arg("ssid"); ssid.trim();
    String pass = s_http.arg("pass");
    if (ssid.length() == 0) { s_http.send(400, "text/plain", "empty ssid"); return; }
    wifiAddNetwork(ssid, pass);
    s_http.sendHeader("Location", "/wifi");
    s_http.send(302);
}

// POST /api/wifi-del (idx) -- remove a saved network.
void handleWifiDel() {
    if (!s_http.hasArg("idx")) { s_http.send(400, "text/plain", "missing idx"); return; }
    int idx = s_http.arg("idx").toInt();
    if (!wifiRemoveNetwork(idx)) { s_http.send(400, "text/plain", "out of range"); return; }
    s_http.sendHeader("Location", "/wifi");
    s_http.send(302);
}

// POST /api/wifi-move (from + to) -- reorder a saved network.
void handleWifiMove() {
    if (!s_http.hasArg("from") || !s_http.hasArg("to")) {
        s_http.send(400, "text/plain", "missing from/to"); return;
    }
    int from = s_http.arg("from").toInt();
    int to   = s_http.arg("to").toInt();
    if (!wifiMoveNetwork(from, to)) {
        s_http.send(400, "text/plain", "out of range"); return;
    }
    s_http.send(200, "text/plain", "ok");
}

// POST /api/wifi-rescan -- trigger a fresh scan, return to /wifi.
void handleWifiRescan() {
    netScanNow();
    s_http.sendHeader("Location", "/wifi");
    s_http.send(302);
}

void handleNotFound() { s_http.send(404, "text/plain", "not found"); }
} // namespace

void webBegin() {
    if (s_running) return;
    if (WiFi.status() != WL_CONNECTED) return;

    // mDNS: announce on the LAN so the radio is reachable at
    // http://on8cit-radio.local without typing the IP.
    if (MDNS.begin(netHostname())) {
        MDNS.addService("http", "tcp", 80);
        MDNS.addServiceTxt("http", "tcp", "version", FIRMWARE_VERSION);
        MDNS.addServiceTxt("http", "tcp", "name",    FIRMWARE_NAME);
        s_mdnsUp = true;
        Serial.printf("web: mDNS up at http://%s.local\r\n", netHostname());
    } else {
        Serial.println("web: mDNS begin FAILED (still reachable via IP)");
    }

    s_http.on("/",             HTTP_GET,  handleIndex);
    s_http.on("/favicon.svg",  HTTP_GET,  handleFavicon);
    s_http.on("/favicon.ico",  HTTP_GET,  handleFavicon);
    s_http.on("/api/state",    HTTP_GET,  handleState);
    s_http.on("/api/station",       HTTP_POST, handleStation);
    s_http.on("/api/station-edit",  HTTP_POST, handleStationEdit);
    s_http.on("/api/station-del",   HTTP_POST, handleStationDel);
    s_http.on("/api/station-move",  HTTP_POST, handleStationMove);
    s_http.on("/api/next",     HTTP_POST, handleNext);
    s_http.on("/api/prev",     HTTP_POST, handlePrev);
    s_http.on("/api/play",     HTTP_POST, handlePlay);
    s_http.on("/discover",     HTTP_GET,  handleDiscover);
    s_http.on("/api/listen",   HTTP_POST, handleListen);
    s_http.on("/api-docs",     HTTP_GET,  handleApiDocs);
    s_http.on("/wifi",         HTTP_GET,  handleWifi);
    s_http.on("/api/wifi-connect",       HTTP_POST, handleWifiConnect);
    s_http.on("/api/wifi-connect-adhoc", HTTP_POST, handleWifiConnectAdhoc);
    s_http.on("/api/wifi-add",           HTTP_POST, handleWifiAdd);
    s_http.on("/api/wifi-del",           HTTP_POST, handleWifiDel);
    s_http.on("/api/wifi-move",          HTTP_POST, handleWifiMove);
    s_http.on("/api/wifi-rescan",        HTTP_POST, handleWifiRescan);
    s_http.on("/api/volume",   HTTP_POST, handleVolume);
    s_http.on("/api/eq",       HTTP_POST, handleEq);
    s_http.on("/api/reboot",   HTTP_POST, handleReboot);
    s_http.on("/api/timezone", HTTP_POST, handleTimezone);
    s_http.onNotFound(handleNotFound);
    s_http.begin();
    s_running = true;
    Serial.printf("web: http server up at http://%s/\r\n", WiFi.localIP().toString().c_str());
}

void webPoll() {
    if (!s_running) return;
    // Record activity whenever a client is connected -- approximates
    // "someone is looking at the web UI", enough to wake the panel.
    if (s_http.client()) displayNoteActivity();
    s_http.handleClient();

    // mDNS heartbeat. WiFi.status() check avoids touching MDNS while the
    // STA interface is disconnected (which would leave the service record
    // orphaned and confuse the next begin()).
    if (WiFi.status() == WL_CONNECTED &&
        (millis() - s_mdnsLastCheckMs) > kMdnsHeartbeatMs) {
        s_mdnsLastCheckMs = millis();
        MDNS.end();
        if (MDNS.begin(netHostname())) {
            MDNS.addService("http", "tcp", 80);
            MDNS.addServiceTxt("http", "tcp", "version", FIRMWARE_VERSION);
            MDNS.addServiceTxt("http", "tcp", "name",    FIRMWARE_NAME);
            s_mdnsUp = true;
        }
    }

    if (s_rebootPending && (int32_t)(millis() - s_rebootAtMs) >= 0) {
        s_http.stop();
        if (s_mdnsUp) MDNS.end();
        delay(100);
        ESP.restart();
    }
}

void webStop() {
    if (!s_running) return;
    s_http.stop();
    if (s_mdnsUp) { MDNS.end(); s_mdnsUp = false; }
    s_running = false;
}

bool webRunning() { return s_running; }
