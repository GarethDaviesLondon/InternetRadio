#include "display.h"
#include "config.h"
#include "radio_audio.h"
#include "net.h"
#include "power.h"
#include "stations.h"
#include "storage.h"
#include "clock.h"
#include "NotoSansBold15.h"

#include <cstdio>
#include <vector>

#include <Arduino_GFX_Library.h>
#include <LovyanGFX.hpp>

// Colour-name compatibility: newer Arduino_GFX / LovyanGFX only ship the
// RGB565_* / TFT_* variants; the original Volos sketch uses the short names.
#ifndef BLACK
  #define BLACK  0x0000
#endif
#ifndef YELLOW
  #define YELLOW 0xFFE0
#endif
#ifndef ORANGE
  #define ORANGE 0xFD20
#endif

// --- Hardware handles -----------------------------------------------------

Theme g_theme = {};

static Arduino_DataBus *s_bus = nullptr;
static Arduino_GFX     *s_gfx = nullptr;
static LGFX_Sprite      s_sprite;
static LGFX_Sprite      s_sprite2;
static int              s_songPosition = -220;
static bool             s_repaint = false;

// Backlight: full brightness while active, dim after kIdleDimMs of no
// user input. Actual duty values are at the low end of the PWM range
// because the Waveshare LCD is bright.
static constexpr uint8_t  kBacklightActive = 110;
static constexpr uint8_t  kBacklightDim    = 20;
static constexpr uint32_t kIdleDimMs       = 60000;   // 60 s
static uint32_t           s_lastActivityMs = 0;
static uint8_t            s_backlightLevel = kBacklightActive;

// Layout constants for the redesigned home screen (240x240).
// Top strip: 0..46 -> banner (brand + battery) + clock row.
// Middle:   48..170 -> now-playing card (left ~90%) + volume column (right).
// Slider:  172..214 -> three-row station switcher with ^/v arrows.
// Footer:  216..240 -> RSSI + bitrate.
// Song-title ticker sits at the very bottom of the now-playing card.
constexpr int kSongScrollX = 6;
constexpr int kSongScrollY = 152;
constexpr int kSongScrollW = 202;
constexpr int kSongScrollH = 14;
// On-air scrolling sub-line (station name from ICY). Runs inside the NP
// card, above the song ticker.
constexpr int kOnAirScrollX = 6;
constexpr int kOnAirScrollY = 112;
constexpr int kOnAirScrollW = 202;
constexpr int kOnAirScrollH = 14;

static int          s_onAirPosition = -220;
static DisplayMode  s_mode = DM_NOW_PLAYING;

// Mode that DM_SYS_INFO / DM_PICKER / DM_WIFI_PICKER were opened from --
// restored on close.
static DisplayMode  s_priorMode = DM_NOW_PLAYING;

// Station-picker cursor: 0..stationsCount()-1. Page = cursor/9.
static int          s_pickerCursor = 0;

// WiFi-picker state: cursor + cached SSID list. SSIDs are captured into
// the cache when Open is called so the grid renders consistently even
// when net's scan vector is mutated by a background re-scan.
static int                  s_wifiCursor = 0;
static std::vector<String>  s_wifiSsids;
static std::vector<int>     s_wifiRssi;

// WiFi connect progress screen.
static String       s_wifiConnectSsid;
static uint32_t     s_wifiConnectElapsedMs = 0;
static String       s_wifiConnectMessage;
static bool         s_wifiConnectFlash = false;

void        displaySetMode(DisplayMode m) { s_mode = m; s_repaint = true; }
void        displayToggleMode()           {
    // Only cycles the two "home" modes. Modal screens (sys-info, picker)
    // are entered by their own gestures and exit explicitly.
    DisplayMode next = (s_mode == DM_NOW_PLAYING) ? DM_BIG_CLOCK : DM_NOW_PLAYING;
    displaySetMode(next);
}
DisplayMode displayActiveMode()           { return s_mode; }

// --- Theme ---------------------------------------------------------------

static void buildDefaultTheme() {
    int co = 214;
    for (int i = 0; i < 18; i++) {
        g_theme.grays[i] = s_sprite.color565(co, co, co + 40);
        co -= 13;
    }
    g_theme.orange      = ORANGE;
    g_theme.panelBg     = BLACK;
    g_theme.panelBorder = g_theme.grays[12];
    g_theme.bg          = g_theme.grays[16];
    g_theme.volumeBar   = YELLOW;
}

void displaySetTheme(const Theme &t) { g_theme = t; }

void displaySetBacklight(uint8_t level) {
    s_backlightLevel = level;
    analogWrite(PIN_LCD_BL, level);
}

uint8_t displayBacklight() { return s_backlightLevel; }

void displayNoteActivity() {
    s_lastActivityMs = millis();
    if (s_backlightLevel != kBacklightActive) {
        displaySetBacklight(kBacklightActive);
    }
}

void displayBacklightTick() {
    if (s_backlightLevel == kBacklightDim) return;
    if ((millis() - s_lastActivityMs) > kIdleDimMs) {
        displaySetBacklight(kBacklightDim);
    }
}

// Parse a colour literal:  0x1F2E  (RGB565),
//                          0xRRGGBB (hex triplet, converted to RGB565),
//                          R,G,B    (decimal, 0..255, converted to RGB565).
static bool parseColor(String v, uint16_t *out) {
    v.trim();
    if (v.length() == 0) return false;
    if (v.startsWith("0x") || v.startsWith("0X")) {
        String hex = v.substring(2);
        unsigned long n = strtoul(hex.c_str(), nullptr, 16);
        if (hex.length() <= 4) {
            *out = (uint16_t)(n & 0xFFFF);
        } else {
            uint8_t r = (n >> 16) & 0xFF;
            uint8_t g = (n >>  8) & 0xFF;
            uint8_t b =  n        & 0xFF;
            *out = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        }
        return true;
    }
    int c1 = v.indexOf(','), c2 = v.indexOf(',', c1 + 1);
    if (c1 > 0 && c2 > 0) {
        int r = v.substring(0, c1).toInt();
        int g = v.substring(c1 + 1, c2).toInt();
        int b = v.substring(c2 + 1).toInt();
        *out = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        return true;
    }
    return false;
}

bool displayLoadThemeFromSd(const char *path) {
    if (!storageSdExists(path)) return false;
    String text;
    if (!storageSdReadText(path, text)) return false;

    Theme t = g_theme;
    int start = 0;
    int applied = 0;
    while (start < (int)text.length()) {
        int nl = text.indexOf('\n', start);
        String line = (nl < 0) ? text.substring(start) : text.substring(start, nl);
        start = (nl < 0) ? text.length() : nl + 1;
        line.trim();
        if (line.length() == 0 || line.startsWith("#") || line.startsWith(";")) continue;
        int eq = line.indexOf('=');
        if (eq <= 0) continue;
        String key = line.substring(0, eq); key.trim(); key.toLowerCase();
        String val = line.substring(eq + 1); val.trim();
        uint16_t c; if (!parseColor(val, &c)) continue;
        if      (key == "bg")          { t.bg          = c; applied++; }
        else if (key == "orange")      { t.orange      = c; applied++; }
        else if (key == "panelbg")     { t.panelBg     = c; applied++; }
        else if (key == "panelborder") { t.panelBorder = c; applied++; }
        else if (key == "volumebar")   { t.volumeBar   = c; applied++; }
    }
    if (applied == 0) return false;
    g_theme = t;
    Serial.printf("display: loaded %d theme key(s) from %s\r\n", applied, path);
    return true;
}

// --- Begin ---------------------------------------------------------------

void displayBegin() {
    s_bus = new Arduino_ESP32SPI(PIN_LCD_DC, PIN_LCD_CS,
                                 PIN_LCD_SCK, PIN_LCD_MOSI, -1);
    s_gfx = new Arduino_ST7789(s_bus, PIN_LCD_RST, 0, true,
                               DISPLAY_W, DISPLAY_H);
    s_gfx->begin();
    s_gfx->fillScreen(RGB565_BLACK);
    analogWrite(PIN_LCD_BL, kBacklightActive);
    s_backlightLevel  = kBacklightActive;
    s_lastActivityMs  = millis();

    s_sprite.setColorDepth(16);
    // Push the 115 KB sprite into PSRAM so the Audio library can claim
    // contiguous DMA-capable internal SRAM.
    s_sprite.setPsram(true);
    s_sprite.createSprite(DISPLAY_W, DISPLAY_H);
    s_sprite2.createSprite(kSongScrollW, kSongScrollH);
    s_sprite.loadFont(NotoSansBold15);

    buildDefaultTheme();
    s_sprite2.setTextColor(g_theme.grays[0], TFT_BLACK);
}

void displayShowMessage(const char *l1, const char *l2,
                        const char *l3, const char *l4) {
    if (!s_gfx) return;
    s_gfx->fillScreen(RGB565_BLACK);
    s_gfx->setCursor(2, 20);
    s_gfx->setTextSize(2);
    s_gfx->setTextColor(RGB565_YELLOW);
    if (l1) s_gfx->println(l1);
    if (l2) s_gfx->println(l2);
    if (l3) s_gfx->println(l3);
    if (l4) s_gfx->println(l4);
}

// ---- Boot-time WiFi screens --------------------------------------------
// These use the panel's built-in bitmap font (via gfx-> directly) rather
// than the sprite's NotoSansBold15 -- SSIDs are longer than the main UI
// text and the built-in font's 6x8 glyphs give us more horizontal room.

namespace {
void drawFooter(const char *footer) {
    if (!footer) return;
    s_gfx->setTextSize(1);
    s_gfx->setTextColor(RGB565_YELLOW);
    s_gfx->setCursor(2, 226);
    s_gfx->print(footer);
}

void rssiBars(int x, int y, int rssi) {
    // 4 bars: 4=good, 3=ok, 2=weak, 1=poor, 0=bad.
    int bars = 0;
    if      (rssi >= -55) bars = 4;
    else if (rssi >= -65) bars = 3;
    else if (rssi >= -75) bars = 2;
    else if (rssi >= -85) bars = 1;
    for (int b = 0; b < 4; b++) {
        int h = 3 + b * 2;
        uint16_t c = (b < bars) ? RGB565_GREEN : 0x39C7;  // dim gray
        s_gfx->fillRect(x + b * 4, y + (10 - h), 3, h, c);
    }
}

// Stylised broadcast/WiFi radiating-waves icon. Drawn via gfx-> directly
// (so it works on boot-time screens before the sprite is busy).
void drawLogo(int cx, int cy, int scale) {
    uint16_t core   = RGB565_YELLOW;
    uint16_t ring1  = RGB565_ORANGE;
    uint16_t ring2  = 0xFCE0;   // amber
    uint16_t ring3  = 0xF7BF;   // faint
    // Three expanding arcs approximated as full thin rings.
    s_gfx->drawCircle(cx, cy, scale * 10, ring3);
    s_gfx->drawCircle(cx, cy, scale * 10 - 1, ring3);
    s_gfx->drawCircle(cx, cy, scale * 7, ring2);
    s_gfx->drawCircle(cx, cy, scale * 7 - 1, ring2);
    s_gfx->drawCircle(cx, cy, scale * 4, ring1);
    s_gfx->drawCircle(cx, cy, scale * 4 - 1, ring1);
    s_gfx->fillCircle(cx, cy, scale * 2, core);
}

// "ON8CIT WebRadio" banner band. Large when used as a splash, compact
// (single row) otherwise. y is the top of the band.
void drawBrandingBanner(int y, bool large) {
    if (large) {
        drawLogo(120, y + 38, 3);
        s_gfx->setTextColor(RGB565_YELLOW);
        s_gfx->setTextSize(4);
        s_gfx->setCursor(36, y + 86);
        s_gfx->print("ON8CIT");
        s_gfx->setTextColor(RGB565_CYAN);
        s_gfx->setTextSize(2);
        s_gfx->setCursor(60, y + 124);
        s_gfx->print("WebRadio");
    } else {
        drawLogo(14, y + 10, 1);
        s_gfx->setTextColor(RGB565_YELLOW);
        s_gfx->setTextSize(2);
        s_gfx->setCursor(30, y + 4);
        s_gfx->print("ON8CIT");
        s_gfx->setTextColor(RGB565_CYAN);
        s_gfx->setCursor(138, y + 4);
        s_gfx->print("WebRadio");
    }
}
} // namespace

void displayShowBootSplash(const char *status) {
    if (!s_gfx) return;
    s_gfx->fillScreen(RGB565_BLACK);
    drawBrandingBanner(0, /*large=*/true);
    s_gfx->setTextSize(1);
    s_gfx->setTextColor(RGB565_WHITE);
    s_gfx->setCursor(2, 174);
    s_gfx->print("Firmware ");
    s_gfx->print(FIRMWARE_VERSION);
    if (status && *status) {
        s_gfx->setCursor(2, 192);
        s_gfx->setTextColor(RGB565_CYAN);
        s_gfx->print(status);
    }
    // Attribution at the bottom of the splash.
    s_gfx->setTextColor(0x8C71 /* muted grey */);
    s_gfx->setCursor(2, 222);
    s_gfx->print("By Gareth Davies, 2026");
}

void displayShowWifiScan(const char *footer, int highlightIdx) {
    if (!s_gfx) return;
    s_gfx->fillScreen(RGB565_BLACK);
    drawBrandingBanner(0, /*large=*/false);
    s_gfx->setTextSize(1);
    s_gfx->setTextColor(RGB565_CYAN);
    s_gfx->setCursor(2, 32);
    s_gfx->print("Visible WiFi networks");

    int n = netScanCount();
    if (n == 0) {
        s_gfx->setTextColor(RGB565_WHITE);
        s_gfx->setCursor(2, 50);
        s_gfx->print("(no networks found)");
        s_gfx->setTextColor(RGB565_YELLOW);
        s_gfx->setCursor(2, 74);
        s_gfx->print("Phone hotspot tips:");
        s_gfx->setTextColor(RGB565_WHITE);
        s_gfx->setCursor(2, 90);
        s_gfx->print("- band: 2.4 GHz (not 5)");
        s_gfx->setCursor(2, 104);
        s_gfx->print("- security: WPA2 or WPA2/3");
        s_gfx->setCursor(2, 118);
        s_gfx->print("  (not WPA3-only)");
        s_gfx->setCursor(2, 132);
        s_gfx->print("- 'Extend compat' if offered");
    } else {
        const int rowH = 14, top = 48, maxRows = (218 - top) / rowH;
        int shown = (n < maxRows) ? n : maxRows;
        for (int i = 0; i < shown; i++) {
            const ScanResult *r = netScanResult(i);
            if (!r) continue;
            int y = top + i * rowH;
            rssiBars(2, y, (int)r->rssi);
            s_gfx->setTextColor(RGB565_CYAN);
            s_gfx->setCursor(22, y + 2);
            s_gfx->printf("ch%-2u", r->channel);
            s_gfx->setTextColor(i == highlightIdx ? RGB565_GREEN : RGB565_WHITE);
            s_gfx->setCursor(52, y + 2);
            String s = r->ssid;
            if (s.length() > 28) s = s.substring(0, 28);
            s_gfx->print(s);
        }
        if (n > maxRows) {
            s_gfx->setTextColor(RGB565_WHITE);
            s_gfx->setCursor(2, top + shown * rowH + 2);
            s_gfx->printf("... +%d more", n - maxRows);
        }
    }
    drawFooter(footer);
}

void displayShowCompactConnect(const char *ssid, int slot, int total,
                               const char *footer) {
    if (!s_gfx) return;
    s_gfx->fillScreen(RGB565_BLACK);
    drawBrandingBanner(0, /*large=*/false);

    // Scan list in the middle third.
    int n = netScanCount();
    const int rowH = 14, top = 32, maxRows = (180 - top) / rowH;
    if (n == 0) {
        s_gfx->setTextColor(RGB565_WHITE);
        s_gfx->setCursor(2, top);
        s_gfx->print("(no networks in cache)");
    } else {
        int shown = (n < maxRows) ? n : maxRows;
        for (int i = 0; i < shown; i++) {
            const ScanResult *r = netScanResult(i);
            if (!r) continue;
            int y = top + i * rowH;
            rssiBars(2, y, (int)r->rssi);
            s_gfx->setTextColor(RGB565_CYAN);
            s_gfx->setCursor(22, y + 2);
            s_gfx->printf("ch%-2u", r->channel);
            s_gfx->setTextColor(RGB565_WHITE);
            s_gfx->setCursor(52, y + 2);
            String s = r->ssid;
            if (s.length() > 28) s = s.substring(0, 28);
            s_gfx->print(s);
        }
        if (n > maxRows) {
            s_gfx->setTextColor(RGB565_WHITE);
            s_gfx->setCursor(2, top + shown * rowH + 2);
            s_gfx->printf("... +%d more", n - maxRows);
        }
    }

    // Commentary (what we're currently trying).
    s_gfx->setTextSize(1);
    s_gfx->setTextColor(RGB565_GREEN);
    s_gfx->setCursor(2, 188);
    if (total > 0 && ssid) {
        s_gfx->printf("Trying %d/%d: ", slot, total);
        s_gfx->setTextColor(RGB565_WHITE);
        String s = ssid;
        // Truncate-with-ellipsis to fit the remaining width (~25 chars).
        if (s.length() > 20) s = s.substring(0, 17) + "...";
        s_gfx->print(s);
    }
    drawFooter(footer);
}

void displayShowConnecting(const char *ssid, int slot, int total,
                           const char *footer) {
    if (!s_gfx) return;
    s_gfx->fillScreen(RGB565_BLACK);
    drawBrandingBanner(0, /*large=*/true);
    s_gfx->setTextSize(1);
    s_gfx->setTextColor(RGB565_GREEN);
    s_gfx->setCursor(2, 180);
    s_gfx->print("Connecting to:");
    s_gfx->setTextColor(RGB565_WHITE);
    s_gfx->setCursor(2, 194);
    s_gfx->print(ssid ? ssid : "?");
    if (total > 0) {
        s_gfx->setTextColor(RGB565_CYAN);
        s_gfx->setCursor(2, 208);
        s_gfx->printf("(slot %d of %d)", slot, total);
    }
    drawFooter(footer);
}

void displayShowSetupMode(const char *apSsid, const char *apIp) {
    if (!s_gfx) return;
    s_gfx->fillScreen(RGB565_BLACK);
    drawBrandingBanner(0, /*large=*/false);
    s_gfx->setTextSize(2);
    s_gfx->setTextColor(RGB565_YELLOW);
    s_gfx->setCursor(2, 34);
    s_gfx->println("WiFi Setup");
    s_gfx->setTextSize(1);
    s_gfx->setTextColor(RGB565_WHITE);
    s_gfx->setCursor(2, 66);
    s_gfx->println("Option A -- serial CLI:");
    s_gfx->setCursor(10, 80);
    s_gfx->setTextColor(RGB565_CYAN);
    s_gfx->println("wifi add");
    s_gfx->setCursor(2, 104);
    s_gfx->setTextColor(RGB565_WHITE);
    s_gfx->println("Option B -- AP portal:");
    s_gfx->setCursor(10, 118);
    s_gfx->setTextColor(RGB565_CYAN);
    s_gfx->print("SSID: "); s_gfx->println(apSsid ? apSsid : "?");
    s_gfx->setCursor(10, 132);
    s_gfx->print("URL : http://on8cit-setup.local");
    s_gfx->setCursor(10, 146);
    s_gfx->print("  or http://radio.setup");
    s_gfx->setCursor(10, 160);
    s_gfx->print("  or http://"); s_gfx->print(apIp ? apIp : "?");
    s_gfx->setTextColor(RGB565_WHITE);
    s_gfx->setCursor(2, 170);
    s_gfx->println("Save a network to exit.");
    drawFooter("Reboot to cancel");
}

void displayRequestRepaint() { s_repaint = true; }
bool displayRepaintPending() { return s_repaint; }

// --- Drawing -------------------------------------------------------------

static void blitSprite(LGFX_Sprite &sp, int dx, int dy, int w, int h) {
    uint16_t *buf = (uint16_t*)sp.getBuffer();
    int total = w * h;
    for (int i = 0; i < total; i++) buf[i] = __builtin_bswap16(buf[i]);
    s_gfx->draw16bitRGBBitmap(dx, dy, buf, w, h);
}

// State cached across calls so we can skip redraws in the static (non-
// scrolling) case -- otherwise the 30 ms tick redraws identical pixels
// and produces a visible flicker on the LCD.
static String s_lastSong;
static int    s_lastSongPos = 0x7FFFFFFF;
static String s_lastOnAir;
static int    s_lastOnAirPos = 0x7FFFFFFF;
static DisplayMode s_lastMode = (DisplayMode)-1;

void displayDrawScroll() {
    // The modal screens own the entire panel; don't overlay the song ticker.
    if (s_mode == DM_SYS_INFO || s_mode == DM_PICKER ||
        s_mode == DM_WIFI_PICKER || s_mode == DM_WIFI_CONNECT ||
        s_mode == DM_STATION_DETAIL) return;

    const uint16_t bg = g_theme.bg;
    const char *song = audioSongPlaying();
    String sSong = song ? song : "";

    if (s_mode == DM_BIG_CLOCK) {
        // Clock mode: full-width ticker painted directly on the panel
        // with the matching bg colour. Placed below the big clock face
        // + date + volume bars so they don't overlap -- see the layout
        // comment in drawBigClock().
        constexpr int kClockSongY = 168;
        constexpr int kClockSongH = 14;
        const int charW = 6;               // Arduino_GFX 5x7 @ size=1
        bool songScroll = ((int)sSong.length() * charW) > 240;
        if (songScroll) {
            s_songPosition--;
            if (s_songPosition < -(int)(sSong.length() * charW)) s_songPosition = 240;
        } else {
            int x = (240 - (int)sSong.length() * charW) / 2;
            if (x < 0) x = 0;
            s_songPosition = x;
        }
        if (songScroll || sSong != s_lastSong || s_lastMode != s_mode) {
            s_gfx->fillRect(0, kClockSongY, 240, kClockSongH, bg);
            s_gfx->setTextWrap(false);         // keep it on one line
            s_gfx->setTextSize(1);
            s_gfx->setTextColor(RGB565_YELLOW, bg);
            s_gfx->setCursor(s_songPosition, kClockSongY + 3);
            s_gfx->print(sSong);
            s_lastSong = sSong;
            s_lastSongPos = s_songPosition;
            s_lastMode = s_mode;
        }
        return;
    }

    // Now-playing mode: both the song title and the on-air station name
    // get sprite-blitted strips. Redraw both only when something
    // actually changed.
    if (s_lastMode != s_mode) {
        // Mode just flipped back into NP; force both strips to repaint.
        s_lastSong = s_lastOnAir = "";
        s_lastSongPos = s_lastOnAirPos = 0x7FFFFFFF;
        s_lastMode = s_mode;
    }

    // Song title strip (always one line, scrolls if long).
    bool songScroll = ((int)sSong.length() * 14) > kSongScrollW;
    if (songScroll) {
        s_songPosition--;
        if (s_songPosition < -(int)(sSong.length() * 14)) s_songPosition = kSongScrollW;
    } else {
        int x = (kSongScrollW - (int)sSong.length() * 14) / 2;
        if (x < 0) x = 0;
        s_songPosition = x;
    }
    if (songScroll || sSong != s_lastSong || s_songPosition != s_lastSongPos) {
        s_sprite2.fillSprite(bg);
        s_sprite2.setTextColor(TFT_YELLOW, bg);
        s_sprite2.drawString(sSong, s_songPosition, 0);
        blitSprite(s_sprite2, kSongScrollX, kSongScrollY, kSongScrollW, kSongScrollH);
        s_lastSong = sSong;
        s_lastSongPos = s_songPosition;
    }

    // On-air station name (only when ICY metadata is available).
    const char *icy = audioCurStation();
    if (!icy || !*icy) {
        if (s_lastOnAir.length()) {
            s_sprite2.fillSprite(bg);
            blitSprite(s_sprite2, kOnAirScrollX, kOnAirScrollY, kOnAirScrollW, kOnAirScrollH);
            s_lastOnAir = "";
        }
        return;
    }
    String sIcy = icy;
    bool onAirScroll = ((int)sIcy.length() * 14) > kOnAirScrollW;
    if (onAirScroll) {
        s_onAirPosition--;
        if (s_onAirPosition < -(int)(sIcy.length() * 14)) s_onAirPosition = kOnAirScrollW;
    } else {
        int x = (kOnAirScrollW - (int)sIcy.length() * 14) / 2;
        if (x < 0) x = 0;
        s_onAirPosition = x;
    }
    if (onAirScroll || sIcy != s_lastOnAir || s_onAirPosition != s_lastOnAirPos) {
        s_sprite2.fillSprite(bg);
        s_sprite2.setTextColor(TFT_CYAN, bg);
        s_sprite2.drawString(sIcy, s_onAirPosition, 0);
        blitSprite(s_sprite2, kOnAirScrollX, kOnAirScrollY, kOnAirScrollW, kOnAirScrollH);
        s_lastOnAir = sIcy;
        s_lastOnAirPos = s_onAirPosition;
    }
}

// Draws only the banner + footer (without clearing the middle). Used by
// both the NP layout and the big-clock layout so the chrome is
// consistent.
static void drawChrome() {
    const uint16_t bg     = g_theme.bg;
    const uint16_t orange = g_theme.orange;
    auto &g = g_theme.grays;

    // ---- Top banner: logo + brand + battery --------------------------
    s_sprite.fillRect(0, 0, 240, 24, TFT_BLACK);
    int lx = 12, ly = 12;
    s_sprite.drawCircle(lx, ly, 10, 0x5220);
    s_sprite.drawCircle(lx, ly,  7, 0xC4C0);
    s_sprite.drawCircle(lx, ly,  4, TFT_YELLOW);
    s_sprite.fillCircle(lx, ly,  2, TFT_YELLOW);
    s_sprite.setTextColor(TFT_YELLOW, TFT_BLACK);
    s_sprite.drawString("ON8CIT",   28, 2, 2);
    s_sprite.setTextColor(TFT_CYAN, TFT_BLACK);
    s_sprite.drawString("WebRadio", 90, 2, 2);
    int batLevel = powerBatteryLevel();
    int bx = 190, by = 6;
    s_sprite.drawRect(bx, by, 40, 12, TFT_GREEN);
    s_sprite.fillRect(bx + 2, by + 2, (batLevel * 36) / 13, 8, TFT_GREEN);
    s_sprite.fillRect(bx + 40, by + 3, 2, 6, TFT_GREEN);

    // ---- Clock strip -------------------------------------------------
    s_sprite.fillRect(0, 24, 240, 22, bg);
    char dateBuf[24], timeBuf[16];
    clockFormatDate(dateBuf, sizeof(dateBuf));
    clockFormatTime(timeBuf, sizeof(timeBuf));
    s_sprite.setTextColor(g[2], bg);
    if (dateBuf[0]) s_sprite.drawString(String(dateBuf), 4,  28, 2);
    else            s_sprite.drawString("(time syncing)",  4, 28, 1);
    s_sprite.setTextColor(TFT_YELLOW, bg);
    if (timeBuf[0]) s_sprite.drawString(String(timeBuf), 166, 28, 2);
    s_sprite.fillRect(0, 46, 240, 1, orange);

    // ---- Footer: RSSI + bitrate -------------------------------------
    s_sprite.fillRect(0, 218, 240, 22, bg);
    s_sprite.fillRect(0, 217, 240, 1, orange);
    char foot[48];
    snprintf(foot, sizeof(foot), "RSSI %d dBm   BITRATE %ld kbps",
             netRssi(), audioBitrate());
    s_sprite.setTextColor(g[2], bg);
    s_sprite.drawString(foot, 6, 223, 1);
}

static void drawNowPlaying() {
    const uint16_t bg     = g_theme.bg;
    const uint16_t light  = g_theme.panelBorder;
    const uint16_t orange = g_theme.orange;
    auto &g = g_theme.grays;

    // ---- Now-playing card (1 px border + 3 px gap below clock divider).
    // Clock divider is at y=46, gap of 3, card starts at 50.
    const int cpX = 4, cpY = 50, cpW = 216, cpH = 122;
    s_sprite.fillRect(cpX, cpY, cpW, cpH, bg);
    s_sprite.drawRect(cpX, cpY, cpW, cpH, light);   // single 1 px border

    int chosen = audioCurrentStation();

    // Big "now playing" name -- ad-hoc preview > ICY broadcast name >
    // current saved slot's display name. Using audioNowPlayingName
    // (not audioStationDisplayName) is what stops the home-page's
    // station grid leaking preview names into slot 0.
    String stName = audioNowPlayingName();
    if (stName.length() > 16) stName = stName.substring(0, 16);
    s_sprite.setTextColor(TFT_GREEN, bg);
    s_sprite.drawString(stName, cpX + 8, cpY + 6, 2);

    // "On Air" / "Now Playing" section labels, left-aligned within the
    // card (user preference -- centred labels looked off-balance next to
    // the big green station name).
    auto leftLabel = [&](const char *txt, int y, uint16_t c) {
        s_sprite.setTextColor(c, bg);
        s_sprite.drawString(txt, cpX + 8, y, 2);
    };
    // Strip coordinates from the top of the file:
    //   kOnAirScrollY = 112  -> label just above it
    //   kSongScrollY  = 152  -> label just above that
    // Labels at y-16 so there's room for font 2's glyph height.
    leftLabel("On Air",     kOnAirScrollY - 18, orange);
    leftLabel("Now Playing", kSongScrollY - 18, orange);

    // NB: no borders around either scroll strip -- user spec.

    // ---- 50 / 50 station switcher -----------------------------------
    // Left half = previous. Right half = next. Title row above each
    // box, station-name inside. Current station is separately shown in
    // the now-playing card, so we don't need to repeat it here.
    const int swY = 174, swH = 42;
    s_sprite.fillRect(0, swY, 240, swH, TFT_BLACK);
    int nsta = stationsCount();
    int prev = (chosen - 1 + nsta) % nsta;
    int next = (chosen + 1) % nsta;
    String nmP = audioStationDisplayName(prev);
    String nmN = audioStationDisplayName(next);
    if (nmP.length() > 13) nmP = nmP.substring(0, 13);
    if (nmN.length() > 13) nmN = nmN.substring(0, 13);

    // Titles, centred in their halves.
    s_sprite.setTextColor(g[6], TFT_BLACK);
    s_sprite.drawString("<<Prev", 34, swY + 2, 1);
    s_sprite.drawString("Next>>", 160, swY + 2, 1);

    // Box for each side.
    s_sprite.drawRect(4,   swY + 14, 114, 22, orange);
    s_sprite.drawRect(122, swY + 14, 114, 22, orange);
    s_sprite.setTextColor(TFT_YELLOW, TFT_BLACK);
    s_sprite.drawString(nmP, 10,  swY + 18, 2);
    s_sprite.drawString(nmN, 128, swY + 18, 2);

    // ---- Volume column on the right edge of the now-playing card.
    // Drawn in the narrow band between the NP card's right border and
    // the display edge (216..240 -> 24 px wide).
    const int vx    = cpX + cpW + 2;        // = 222
    const int vyTop = cpY + 4,  vyBot = cpY + cpH - 4;
    s_sprite.fillTriangle(vx + 7, vyTop,       vx,     vyTop + 8, vx + 14, vyTop + 8, g[2]);
    s_sprite.fillTriangle(vx + 7, vyBot,       vx,     vyBot - 8, vx + 14, vyBot - 8, g[2]);
    int vol = audioVolume();
    const int segX = vx + 3, segW = 8;
    const int segTop = vyTop + 14, segBot = vyBot - 14;
    const int segTotal = segBot - segTop;
    const int gap = 2, segH = (segTotal - 4 * gap) / 5;
    for (int i = 0; i < 5; i++) {
        int y = segBot - (i + 1) * segH - i * gap;
        uint16_t c = (i < vol) ? g_theme.volumeBar : g[11];
        s_sprite.fillRect(segX, y, segW, segH, c);
    }

    // ---- Paused overlay (NP mode only). Big "||" in the card centre.
    if (audioIsPaused()) {
        int cx = cpX + cpW / 2;
        int cy = cpY + cpH / 2;
        const int bw = 18, bh = 54, gap2 = 14;
        s_sprite.fillRoundRect(cx - gap2/2 - bw, cy - bh/2, bw, bh, 3, TFT_YELLOW);
        s_sprite.fillRoundRect(cx + gap2/2,      cy - bh/2, bw, bh, 3, TFT_YELLOW);
    }
}

static void drawBigClock() {
    const uint16_t bg = g_theme.bg;
    auto &g = g_theme.grays;

    // Clear the middle band (clock+NP+switcher area) without touching
    // the top banner/clock strip or the bottom footer.
    s_sprite.fillRect(0, 50, 240, 166, bg);

    char timeBuf[16];
    clockFormatTime(timeBuf, sizeof(timeBuf));
    if (!timeBuf[0]) {
        s_sprite.setTextColor(TFT_YELLOW, bg);
        s_sprite.drawString("(syncing)", 60, 110, 4);
        return;
    }
    // HH:MM:SS in the big LGFX font 7 (7-segment LCD look). LGFX
    // drawString doesn't auto-wrap the sprite, so overflow isn't a
    // problem; we just centre it horizontally.
    s_sprite.setTextColor(TFT_YELLOW, bg);
    {
        String t = timeBuf;
        s_sprite.setTextFont(7);
        int tw = s_sprite.textWidth(t);
        int lx = (240 - tw) / 2; if (lx < 0) lx = 0;
        s_sprite.drawString(t, lx, 58);
    }

    // Date in smaller bold below.
    char dateBuf[24];
    clockFormatDate(dateBuf, sizeof(dateBuf));
    s_sprite.setTextColor(TFT_CYAN, bg);
    {
        String d = dateBuf;
        s_sprite.setTextFont(2);
        int dw = s_sprite.textWidth(d);
        int lx = (240 - dw) / 2; if (lx < 0) lx = 0;
        s_sprite.drawString(d, lx, 128);
    }

    // ---- Song title scroll strip (user: "There is no now-playing
    // information scrolling at all"). displayDrawScroll() renders it
    // at kClockSongY (see displayDrawScroll). Needs to be BELOW the
    // date and ABOVE the volume bar -- the old layout had the strip
    // behind the volume control.
    // kClockSongY is now 168 (see top of file); stays visible.

    // ---- Paused indicator: small "||" + "Playback Paused" text.
    // Just below the song ticker and above the new small volume bar.
    if (audioIsPaused()) {
        const char *label = "Playback Paused";
        const int labelW  = (int)strlen(label) * 6;
        const int bars    = 13;
        const int total   = bars + 4 + labelW;
        const int sx      = (240 - total) / 2;
        const int y       = 186;
        s_sprite.fillRoundRect(sx,     y, 5, 14, 1, TFT_YELLOW);
        s_sprite.fillRoundRect(sx + 8, y, 5, 14, 1, TFT_YELLOW);
        s_sprite.setTextColor(TFT_YELLOW, bg);
        s_sprite.drawString(label, sx + bars + 4, y + 3, 1);
    }

    // ---- Small horizontal volume bar at the very bottom (just above
    // the orange-rule footer at y=217). Smaller height (8 px) and
    // narrower segments so it doesn't dominate the clock face.
    int vol = audioVolume();
    {
        const int segW = 30, segH = 8, gap = 3;
        const int total = 5 * segW + 4 * gap;
        const int sx = (240 - total) / 2;
        const int sy = 207;
        for (int i = 0; i < 5; i++) {
            uint16_t c = (i < vol) ? g_theme.volumeBar : g[11];
            s_sprite.fillRoundRect(sx + i * (segW + gap), sy, segW, segH, 2, c);
        }
        // "VOL" label left-justified, above the bar so it doesn't
        // steal the bar's width.
        s_sprite.setTextColor(g[6], bg);
        s_sprite.drawString("VOL", sx, sy - 10, 1);
    }
}

// ---------------------------------------------------------------------------
// System-info screen (long-press Right button). Battery, WiFi SSID, RSSI,
// IP. Exits on any short press in the main-loop dispatcher.
// ---------------------------------------------------------------------------
static void drawSysInfo() {
    const uint16_t bg     = g_theme.bg;
    const uint16_t orange = g_theme.orange;
    auto &g = g_theme.grays;

    s_sprite.fillRect(0, 0, 240, 240, bg);

    // Header band.
    s_sprite.fillRect(0, 0, 240, 28, TFT_BLACK);
    s_sprite.setTextColor(TFT_YELLOW, TFT_BLACK);
    s_sprite.drawString("System info", 6, 6, 2);
    s_sprite.fillRect(0, 28, 240, 1, orange);

    // Battery.
    s_sprite.setTextColor(g[2], bg);
    s_sprite.drawString("Battery", 8, 40, 2);
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f V", powerBatteryVolts());
    s_sprite.setTextColor(TFT_YELLOW, bg);
    s_sprite.drawString(buf, 130, 40, 2);

    int batLevel = powerBatteryLevel();
    int bx = 8, by = 64, bw = 224, bh = 14;
    s_sprite.drawRect(bx, by, bw, bh, TFT_GREEN);
    s_sprite.fillRect(bx + 2, by + 2, ((bw - 4) * batLevel) / 13, bh - 4, TFT_GREEN);

    // WiFi SSID.
    s_sprite.setTextColor(g[2], bg);
    s_sprite.drawString("WiFi", 8, 92, 2);
    String ssid = netCurrentSsid();
    if (ssid.length() == 0) ssid = "(disconnected)";
    if (ssid.length() > 20) ssid = ssid.substring(0, 20);
    s_sprite.setTextColor(TFT_CYAN, bg);
    s_sprite.drawString(ssid, 60, 92, 2);

    // Signal strength as a 5-bar meter + dBm.
    int rssi = netRssi();
    int bars = 0;
    if      (rssi >= -55) bars = 5;
    else if (rssi >= -65) bars = 4;
    else if (rssi >= -72) bars = 3;
    else if (rssi >= -80) bars = 2;
    else if (rssi >= -90) bars = 1;
    s_sprite.setTextColor(g[2], bg);
    s_sprite.drawString("Signal", 8, 120, 2);
    {
        const int sx = 80, sy = 122, segW = 12, segH = 14, gap = 4;
        for (int i = 0; i < 5; i++) {
            uint16_t c = (i < bars) ? TFT_YELLOW : g[11];
            s_sprite.fillRoundRect(sx + i * (segW + gap), sy + (4 - i) * 0,
                                   segW, segH, 2, c);
        }
    }
    snprintf(buf, sizeof(buf), "%d dBm", rssi);
    s_sprite.setTextColor(TFT_YELLOW, bg);
    s_sprite.drawString(buf, 168, 120, 2);

    // IP address.
    s_sprite.setTextColor(g[2], bg);
    s_sprite.drawString("IP", 8, 148, 2);
    String ip = netLocalIp();
    if (ip == "0.0.0.0") ip = "(none)";
    s_sprite.setTextColor(TFT_CYAN, bg);
    s_sprite.drawString(ip, 40, 148, 2);

    // Hostname.
    s_sprite.setTextColor(g[2], bg);
    s_sprite.drawString("Host", 8, 172, 2);
    String host = String(netHostname()) + ".local";
    s_sprite.setTextColor(TFT_CYAN, bg);
    s_sprite.drawString(host, 60, 172, 2);

    // Footer hint.
    s_sprite.fillRect(0, 217, 240, 1, orange);
    s_sprite.setTextColor(g[6], bg);
    s_sprite.drawString("Press any button to exit", 6, 222, 1);
}

// ---------------------------------------------------------------------------
// Station picker (long-press Mid). 3x3 grid of cells, paged. Mid short
// advances the cursor; mid double-click selects; left/right short exit.
// ---------------------------------------------------------------------------
static void drawPicker() {
    const uint16_t bg     = g_theme.bg;
    const uint16_t orange = g_theme.orange;
    auto &g = g_theme.grays;

    s_sprite.fillRect(0, 0, 240, 240, bg);

    int total = stationsCount();
    if (s_pickerCursor < 0)        s_pickerCursor = 0;
    if (s_pickerCursor >= total)   s_pickerCursor = total ? total - 1 : 0;

    int page    = (total > 0) ? (s_pickerCursor / 9) : 0;
    int pages   = (total > 0) ? ((total + 8) / 9)   : 1;
    int first   = page * 9;
    int last    = min(first + 9, total) - 1;

    // Header.
    s_sprite.fillRect(0, 0, 240, 28, TFT_BLACK);
    s_sprite.setTextColor(TFT_YELLOW, TFT_BLACK);
    s_sprite.drawString("Pick station", 6, 6, 2);
    char hdr[24];
    if (total == 0) {
        snprintf(hdr, sizeof(hdr), "(empty)");
    } else {
        snprintf(hdr, sizeof(hdr), "%d-%d / %d", first + 1, last + 1, total);
    }
    s_sprite.setTextColor(TFT_CYAN, TFT_BLACK);
    s_sprite.drawString(hdr, 144, 6, 2);
    s_sprite.fillRect(0, 28, 240, 1, orange);

    // 3x3 cell grid: full 240x180 bottom area.
    constexpr int gridY = 30, gridH = 184;     // 30..214
    constexpr int cellW = 78, cellH = 60, gx = 2, gy = 32;
    for (int i = 0; i < 9; i++) {
        int row = i / 3;
        int col = i % 3;
        int cx  = gx + col * (cellW + 2);
        int cy  = gy + row * (cellH + 2);

        int slotIdx = first + i;
        bool inUse  = slotIdx < total;
        bool isCur  = inUse && (slotIdx == s_pickerCursor);
        bool isPlay = inUse && (slotIdx == audioCurrentStation());

        uint16_t cellBg = isCur ? g_theme.volumeBar
                          : (isPlay ? 0x4A69 : g[14]);
        uint16_t cellBd = isCur ? TFT_YELLOW : g[10];
        uint16_t cellTx = isCur ? TFT_BLACK  : (inUse ? TFT_WHITE : g[6]);

        s_sprite.fillRoundRect(cx, cy, cellW, cellH, 4, cellBg);
        s_sprite.drawRoundRect(cx, cy, cellW, cellH, 4, cellBd);
        if (isCur) s_sprite.drawRoundRect(cx + 1, cy + 1, cellW - 2, cellH - 2, 4, TFT_YELLOW);

        // Slot number top-left.
        char num[8];
        snprintf(num, sizeof(num), "%d", slotIdx + 1);
        s_sprite.setTextColor(cellTx, cellBg);
        s_sprite.drawString(num, cx + 4, cy + 4, 2);

        // Station name, broken into up to 2 visual rows of 10 chars.
        if (inUse) {
            String nm = audioStationDisplayName(slotIdx);
            // Trim to 22 chars total (2 rows of 11).
            if (nm.length() > 22) nm = nm.substring(0, 22);
            String row1 = nm, row2;
            if (nm.length() > 11) {
                int br = nm.lastIndexOf(' ', 11);
                if (br < 4) br = 11;
                row1 = nm.substring(0, br);
                row2 = nm.substring(br);
                row1.trim(); row2.trim();
            }
            s_sprite.drawString(row1, cx + 4, cy + 24, 1);
            if (row2.length()) s_sprite.drawString(row2, cx + 4, cy + 36, 1);
        }
    }

    // Footer hint.
    s_sprite.fillRect(0, 217, 240, 1, orange);
    s_sprite.setTextColor(g[6], bg);
    if (total == 0) {
        s_sprite.drawString("No stations -- L: exit", 6, 222, 1);
    } else if (pages > 1) {
        char hint[64];
        snprintf(hint, sizeof(hint),
                 "R: next  M: prev  hold/x2: pick  L: exit  p%d/%d",
                 page + 1, pages);
        s_sprite.drawString(hint, 6, 222, 1);
    } else {
        s_sprite.drawString("R: next  M: prev  hold/x2: pick  L: exit",
                            6, 222, 1);
    }
}

// ---------------------------------------------------------------------------
// Picker public control surface. Open / advance / close are called by
// the main loop's input dispatcher; selection commit is also routed
// through the dispatcher so it can call audioSelectStation + close.
// ---------------------------------------------------------------------------
static void rememberPrior() {
    if (s_mode != DM_PICKER && s_mode != DM_SYS_INFO) s_priorMode = s_mode;
}

void displaySysInfoOpen() {
    rememberPrior();
    displaySetMode(DM_SYS_INFO);
}

void displayStationDetailOpen() {
    rememberPrior();
    displaySetMode(DM_STATION_DETAIL);
}

void displayPickerOpen() {
    rememberPrior();
    s_pickerCursor = audioCurrentStation();
    if (s_pickerCursor < 0 || s_pickerCursor >= stationsCount()) s_pickerCursor = 0;
    displaySetMode(DM_PICKER);
}

void displayPickerAdvance() {
    int n = stationsCount();
    if (n <= 0) return;
    s_pickerCursor++;
    if (s_pickerCursor >= n) s_pickerCursor = 0;
    s_repaint = true;
}

void displayPickerRetreat() {
    int n = stationsCount();
    if (n <= 0) return;
    s_pickerCursor--;
    if (s_pickerCursor < 0) s_pickerCursor = n - 1;
    s_repaint = true;
}

int displayPickerSelectedSlot() {
    if (stationsCount() <= 0) return -1;
    return s_pickerCursor;
}

void displayModalClose() {
    DisplayMode home = s_priorMode;
    if (home == DM_PICKER || home == DM_SYS_INFO ||
        home == DM_WIFI_PICKER || home == DM_WIFI_CONNECT ||
        home == DM_STATION_DETAIL) {
        home = DM_NOW_PLAYING;
    }
    displaySetMode(home);
}

// ---------------------------------------------------------------------------
// WiFi picker. 3x3 grid of scanned SSIDs. Same nav UX as the station
// picker. Opening triggers a fresh scan and snapshots the SSID list so
// the grid is stable while the user navigates it.
// ---------------------------------------------------------------------------

void displayWifiPickerOpen() {
    rememberPrior();
    // Show a quick "Scanning..." message synchronously so the user
    // doesn't wait for the ~2-3 s blocking scan with stale UI.
    displayShowMessage("Scanning WiFi...", "Hold tight.", nullptr);
    s_wifiSsids.clear();
    s_wifiRssi.clear();
    int n = netScanNow();
    for (int i = 0; i < n; i++) {
        const ScanResult *r = netScanResult(i);
        if (!r) continue;
        if (r->ssid.length() == 0) continue;
        s_wifiSsids.push_back(r->ssid);
        s_wifiRssi.push_back(r->rssi);
    }
    s_wifiCursor = 0;
    displaySetMode(DM_WIFI_PICKER);
}

void displayWifiPickerAdvance() {
    if (s_wifiSsids.empty()) return;
    s_wifiCursor++;
    if (s_wifiCursor >= (int)s_wifiSsids.size()) s_wifiCursor = 0;
    s_repaint = true;
}
int displayWifiPickerSelected() {
    if (s_wifiSsids.empty()) return -1;
    return s_wifiCursor;
}
int displayWifiPickerSsidCount() { return (int)s_wifiSsids.size(); }
const char *displayWifiPickerSsidAt(int i) {
    if (i < 0 || i >= (int)s_wifiSsids.size()) return "";
    return s_wifiSsids[i].c_str();
}

static void drawWifiPicker() {
    const uint16_t bg     = g_theme.bg;
    const uint16_t orange = g_theme.orange;
    auto &g = g_theme.grays;

    s_sprite.fillRect(0, 0, 240, 240, bg);

    int total = (int)s_wifiSsids.size();
    if (s_wifiCursor < 0)      s_wifiCursor = 0;
    if (s_wifiCursor >= total) s_wifiCursor = total ? total - 1 : 0;
    int page  = (total > 0) ? (s_wifiCursor / 9) : 0;
    int pages = (total > 0) ? ((total + 8) / 9) : 1;
    int first = page * 9;
    int last  = min(first + 9, total) - 1;

    s_sprite.fillRect(0, 0, 240, 28, TFT_BLACK);
    s_sprite.setTextColor(TFT_YELLOW, TFT_BLACK);
    s_sprite.drawString("Pick WiFi", 6, 6, 2);
    char hdr[24];
    if (total == 0) snprintf(hdr, sizeof(hdr), "(no scan)");
    else            snprintf(hdr, sizeof(hdr), "%d-%d / %d", first + 1, last + 1, total);
    s_sprite.setTextColor(TFT_CYAN, TFT_BLACK);
    s_sprite.drawString(hdr, 144, 6, 2);
    s_sprite.fillRect(0, 28, 240, 1, orange);

    constexpr int cellW = 78, cellH = 60, gx = 2, gy = 32;
    for (int i = 0; i < 9; i++) {
        int row = i / 3, col = i % 3;
        int cx  = gx + col * (cellW + 2);
        int cy  = gy + row * (cellH + 2);
        int idx = first + i;
        bool inUse = idx < total;
        bool isCur = inUse && (idx == s_wifiCursor);

        uint16_t cellBg = isCur ? g_theme.volumeBar : g[14];
        uint16_t cellBd = isCur ? TFT_YELLOW : g[10];
        uint16_t cellTx = isCur ? TFT_BLACK  : (inUse ? TFT_WHITE : g[6]);

        s_sprite.fillRoundRect(cx, cy, cellW, cellH, 4, cellBg);
        s_sprite.drawRoundRect(cx, cy, cellW, cellH, 4, cellBd);
        if (isCur) s_sprite.drawRoundRect(cx + 1, cy + 1, cellW - 2, cellH - 2, 4, TFT_YELLOW);

        if (inUse) {
            // Signal-strength bars top-right.
            int rssi = s_wifiRssi[idx];
            int bars = 0;
            if      (rssi >= -55) bars = 4;
            else if (rssi >= -65) bars = 3;
            else if (rssi >= -75) bars = 2;
            else if (rssi >= -85) bars = 1;
            for (int b = 0; b < 4; b++) {
                int barH = 4 + b * 2;
                int bx = cx + cellW - 22 + b * 5;
                int by = cy + 4 + (10 - barH);
                uint16_t c = (b < bars) ? cellTx : g[10];
                s_sprite.fillRect(bx, by, 3, barH, c);
            }
            // SSID name, two-line wrap if needed.
            String nm = s_wifiSsids[idx];
            if (nm.length() > 22) nm = nm.substring(0, 22);
            String row1 = nm, row2;
            if (nm.length() > 11) {
                int br = nm.lastIndexOf(' ', 11);
                if (br < 4) br = 11;
                row1 = nm.substring(0, br);
                row2 = nm.substring(br);
                row1.trim(); row2.trim();
            }
            s_sprite.setTextColor(cellTx, cellBg);
            s_sprite.drawString(row1, cx + 4, cy + 22, 1);
            if (row2.length()) s_sprite.drawString(row2, cx + 4, cy + 36, 1);
        }
    }

    s_sprite.fillRect(0, 217, 240, 1, orange);
    s_sprite.setTextColor(g[6], bg);
    if (total == 0) {
        s_sprite.drawString("Scan empty -- L/R: exit", 6, 222, 1);
    } else if (pages > 1) {
        char hint[48];
        snprintf(hint, sizeof(hint),
                 "Mid: next  Mid x2: connect  L/R: exit  p%d/%d",
                 page + 1, pages);
        s_sprite.drawString(hint, 6, 222, 1);
    } else {
        s_sprite.drawString("Mid: next  Mid x2: connect  L/R: exit",
                            6, 222, 1);
    }
}

// ---------------------------------------------------------------------------
// Station details screen (Left double-click in Now Playing). Shows the
// current slot's friendly name, full stream URL (wrapped to fit the
// 240 px width), ICY broadcast name, codec / bitrate, and current song.
// ---------------------------------------------------------------------------
namespace {
// Word-wrap helper: break a long string into lines of <= max chars,
// preferring whitespace breaks. URLs without spaces just hard-wrap.
void wrapLines(const String &s, int maxChars, std::vector<String> &out) {
    int i = 0, n = s.length();
    while (i < n) {
        int end = i + maxChars;
        if (end >= n) { out.push_back(s.substring(i)); break; }
        int br = -1;
        for (int j = end; j > i; j--) {
            char c = s[j];
            if (c == ' ' || c == '/' || c == '?' || c == '&' || c == '=' ) { br = j + 1; break; }
        }
        if (br < 0 || br <= i) br = end;
        out.push_back(s.substring(i, br));
        i = br;
        while (i < n && s[i] == ' ') i++;
    }
}
} // namespace

static void drawStationDetail() {
    const uint16_t bg     = g_theme.bg;
    const uint16_t orange = g_theme.orange;
    auto &g = g_theme.grays;

    s_sprite.fillRect(0, 0, 240, 240, bg);
    s_sprite.fillRect(0, 0, 240, 28, TFT_BLACK);
    s_sprite.setTextColor(TFT_YELLOW, TFT_BLACK);
    s_sprite.drawString("Station details", 6, 6, 2);
    s_sprite.fillRect(0, 28, 240, 1, orange);

    int slot = audioCurrentStation();
    int n    = stationsCount();

    // Slot indicator + friendly name.
    s_sprite.setTextColor(g[2], bg);
    s_sprite.drawString("Slot", 8, 36, 2);
    char idx[16];
    snprintf(idx, sizeof(idx), "%d / %d", slot + 1, n);
    s_sprite.setTextColor(TFT_CYAN, bg);
    s_sprite.drawString(idx, 56, 36, 2);

    s_sprite.setTextColor(g[2], bg);
    s_sprite.drawString("Name", 8, 60, 2);
    String nm = audioStationDisplayName(slot);
    if (nm.length() > 28) nm = nm.substring(0, 28);
    s_sprite.setTextColor(TFT_YELLOW, bg);
    s_sprite.drawString(nm, 56, 60, 2);

    // ICY station name (if the stream sent one).
    String icy = audioCurStation();
    if (icy.length() && icy != nm) {
        s_sprite.setTextColor(g[2], bg);
        s_sprite.drawString("On air", 8, 84, 2);
        if (icy.length() > 26) icy = icy.substring(0, 26);
        s_sprite.setTextColor(TFT_CYAN, bg);
        s_sprite.drawString(icy, 70, 84, 2);
    }

    // URL (wrapped). At font 1 (~6 px/char) we fit ~38 chars/240 px.
    s_sprite.setTextColor(g[2], bg);
    s_sprite.drawString("URL", 8, 110, 1);
    String url = stationsUrl(slot);
    std::vector<String> lines;
    wrapLines(url, 38, lines);
    int yy = 122;
    int maxLines = 4;
    s_sprite.setTextColor(g[2], bg);
    for (int i = 0; i < (int)lines.size() && i < maxLines; i++) {
        s_sprite.drawString(lines[i], 8, yy, 1);
        yy += 10;
    }
    if ((int)lines.size() > maxLines) {
        s_sprite.drawString("...", 8, yy, 1);
    }

    // Codec / bitrate.
    long br = audioBitrate();
    char info[32];
    if (br > 0) snprintf(info, sizeof(info), "%ld kbps", br);
    else        snprintf(info, sizeof(info), "(unknown)");
    s_sprite.setTextColor(g[2], bg);
    s_sprite.drawString("Bitrate", 8, 178, 2);
    s_sprite.setTextColor(TFT_YELLOW, bg);
    s_sprite.drawString(info, 80, 178, 2);

    // Current song line at the bottom.
    String song = audioSongPlaying();
    if (song.length() > 38) song = song.substring(0, 38);
    s_sprite.setTextColor(g[6], bg);
    s_sprite.drawString(song, 8, 198, 1);

    // Footer hint.
    s_sprite.fillRect(0, 217, 240, 1, orange);
    s_sprite.setTextColor(g[6], bg);
    s_sprite.drawString("Any short press to exit", 6, 222, 1);
}

// ---------------------------------------------------------------------------
// "Connecting to <ssid>..." progress screen.
// ---------------------------------------------------------------------------

void displayWifiConnectShow(const char *ssid) {
    rememberPrior();
    s_wifiConnectSsid       = ssid ? ssid : "";
    s_wifiConnectElapsedMs  = 0;
    s_wifiConnectMessage    = "";
    s_wifiConnectFlash      = false;
    displaySetMode(DM_WIFI_CONNECT);
}
void displayWifiConnectTick(uint32_t elapsedMs) {
    // We're called from inside the blocking netConnectAdhoc loop, so
    // the main loop's repaint dispatcher isn't running. Draw inline,
    // throttled to ~5 Hz so we don't melt the SPI bus.
    static uint32_t lastDraw = 0;
    s_wifiConnectElapsedMs = elapsedMs;
    if (elapsedMs - lastDraw < 200 && lastDraw != 0) return;
    lastDraw = elapsedMs;
    if (s_mode == DM_WIFI_CONNECT) displayDrawMain();
}
void displayWifiConnectFail(const char *reason) {
    s_wifiConnectMessage = reason ? reason : "Connect failed.";
    s_wifiConnectFlash   = true;
    s_repaint = true;
}
void displayWifiConnectDone(bool ok) {
    if (ok) {
        // Success: jump to Now Playing regardless of where we came from.
        displaySetMode(DM_NOW_PLAYING);
    } else {
        displaySetMode(DM_WIFI_PICKER);
    }
}

static void drawWifiConnect() {
    const uint16_t bg     = g_theme.bg;
    const uint16_t orange = g_theme.orange;
    auto &g = g_theme.grays;

    s_sprite.fillRect(0, 0, 240, 240, bg);
    s_sprite.fillRect(0, 0, 240, 28, TFT_BLACK);
    s_sprite.setTextColor(TFT_YELLOW, TFT_BLACK);
    s_sprite.drawString("Connecting", 6, 6, 2);
    s_sprite.fillRect(0, 28, 240, 1, orange);

    s_sprite.setTextColor(g[2], bg);
    s_sprite.drawString("To:", 8, 50, 2);
    String nm = s_wifiConnectSsid;
    if (nm.length() > 22) nm = nm.substring(0, 22);
    s_sprite.setTextColor(TFT_CYAN, bg);
    s_sprite.drawString(nm, 60, 50, 2);

    // Growing dots indicator.
    int dots = (s_wifiConnectElapsedMs / 250) % 16;
    String d;
    for (int i = 0; i < dots; i++) d += '.';
    s_sprite.setTextColor(TFT_YELLOW, bg);
    s_sprite.drawString(d, 8, 90, 4);

    // Elapsed time.
    char buf[24];
    snprintf(buf, sizeof(buf), "%lus elapsed", (unsigned long)(s_wifiConnectElapsedMs / 1000));
    s_sprite.setTextColor(g[2], bg);
    s_sprite.drawString(buf, 8, 150, 2);

    if (s_wifiConnectMessage.length()) {
        s_sprite.setTextColor(TFT_RED, bg);
        s_sprite.drawString(s_wifiConnectMessage, 8, 178, 2);
    }

    s_sprite.fillRect(0, 217, 240, 1, orange);
    s_sprite.setTextColor(g[6], bg);
    s_sprite.drawString("Please wait...", 6, 222, 1);
}

void displayDrawMain() {
    s_sprite.fillRect(0, 0, 240, 240, g_theme.bg);

    if (s_mode == DM_SYS_INFO) {
        drawSysInfo();
        blitSprite(s_sprite, 0, 0, DISPLAY_W, DISPLAY_H);
        s_repaint = false;
        return;
    }
    if (s_mode == DM_PICKER) {
        drawPicker();
        blitSprite(s_sprite, 0, 0, DISPLAY_W, DISPLAY_H);
        s_repaint = false;
        return;
    }
    if (s_mode == DM_WIFI_PICKER) {
        drawWifiPicker();
        blitSprite(s_sprite, 0, 0, DISPLAY_W, DISPLAY_H);
        s_repaint = false;
        return;
    }
    if (s_mode == DM_WIFI_CONNECT) {
        drawWifiConnect();
        blitSprite(s_sprite, 0, 0, DISPLAY_W, DISPLAY_H);
        s_repaint = false;
        return;
    }
    if (s_mode == DM_STATION_DETAIL) {
        drawStationDetail();
        blitSprite(s_sprite, 0, 0, DISPLAY_W, DISPLAY_H);
        s_repaint = false;
        return;
    }

    drawChrome();
    if (s_mode == DM_BIG_CLOCK) drawBigClock();
    else                         drawNowPlaying();

    blitSprite(s_sprite, 0, 0, DISPLAY_W, DISPLAY_H);
    s_repaint = false;

    // Refresh the song-title ticker too -- the main repaint clears it.
    displayDrawScroll();
}
