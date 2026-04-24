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
constexpr int kSongScrollX = 6;
constexpr int kSongScrollY = 150;
constexpr int kSongScrollW = 202;
constexpr int kSongScrollH = 16;

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
    s_gfx->setCursor(2, 180);
    s_gfx->print("Firmware ");
    s_gfx->print(FIRMWARE_VERSION);
    if (status && *status) {
        s_gfx->setCursor(2, 200);
        s_gfx->setTextColor(RGB565_CYAN);
        s_gfx->print(status);
    }
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

void displayDrawScroll() {
    s_songPosition--;
    if (s_songPosition < -220) s_songPosition = kSongScrollW;
    s_sprite2.fillSprite(TFT_BLACK);
    s_sprite2.drawString(audioSongPlaying(), s_songPosition, 5);
    blitSprite(s_sprite2, kSongScrollX, kSongScrollY, kSongScrollW, kSongScrollH);
}

void displayDrawMain() {
    const uint16_t bg     = g_theme.bg;
    const uint16_t light  = g_theme.panelBorder;
    const uint16_t orange = g_theme.orange;
    auto &g = g_theme.grays;

    s_sprite.fillRect(0, 0, 240, 240, bg);

    // ---- Top banner: logo + brand + battery --------------------------
    // Mini broadcast-waves logo (concentric rings + core) on the left.
    s_sprite.fillRect(0, 0, 240, 24, TFT_BLACK);
    int lx = 12, ly = 12;
    s_sprite.drawCircle(lx, ly, 10, 0x5220);                    // faint ring
    s_sprite.drawCircle(lx, ly,  7, 0xC4C0);                    // mid ring
    s_sprite.drawCircle(lx, ly,  4, TFT_YELLOW);                // core ring
    s_sprite.fillCircle(lx, ly,  2, TFT_YELLOW);
    s_sprite.setTextColor(TFT_YELLOW, TFT_BLACK);
    s_sprite.drawString("ON8CIT",   28, 2, 2);
    s_sprite.setTextColor(TFT_CYAN, TFT_BLACK);
    s_sprite.drawString("WebRadio", 90, 2, 2);

    // Battery pill at the right edge of the banner.
    int batLevel = powerBatteryLevel();   // 0..13
    int bx = 190, by = 6;
    s_sprite.drawRect(bx, by, 40, 12, TFT_GREEN);
    s_sprite.fillRect(bx + 2, by + 2, (batLevel * 36) / 13, 8, TFT_GREEN);
    s_sprite.fillRect(bx + 40, by + 3, 2, 6, TFT_GREEN);        // nub

    // ---- Clock strip (day date-month + HH:MM:SS) ---------------------
    s_sprite.fillRect(0, 24, 240, 22, bg);
    char dateBuf[24], timeBuf[16];
    clockFormatDate(dateBuf, sizeof(dateBuf));
    clockFormatTime(timeBuf, sizeof(timeBuf));
    s_sprite.setTextColor(g[2], bg);
    if (dateBuf[0]) s_sprite.drawString(String(dateBuf), 4,  28, 2);
    else            s_sprite.drawString("(time syncing)",  4, 28, 1);
    s_sprite.setTextColor(TFT_YELLOW, bg);
    if (timeBuf[0]) s_sprite.drawString(String(timeBuf), 166, 28, 2);

    // Divider under the clock.
    s_sprite.fillRect(0, 46, 240, 1, orange);

    // ---- Now-playing card (left 90%) + volume column (right) ---------
    const int cpX = 4, cpY = 50, cpW = 206, cpH = 122;
    s_sprite.fillRect(cpX, cpY, cpW, cpH, TFT_BLACK);
    s_sprite.drawRect(cpX, cpY, cpW, cpH, light);

    s_sprite.setTextColor(orange, TFT_BLACK);
    s_sprite.drawString("NOW PLAYING", cpX + 8, cpY + 4, 1);

    int chosen = audioCurrentStation();
    int nsta   = stationsCount();

    // Big display name (override > ICY > URL-derived).
    String stName = audioStationDisplayName(chosen);
    if (stName.length() > 16) stName = stName.substring(0, 16);
    s_sprite.setTextColor(TFT_GREEN, TFT_BLACK);
    s_sprite.drawString(stName, cpX + 8, cpY + 24, 2);

    // Bitrate / slot row.
    char sub[32];
    snprintf(sub, sizeof(sub), "Slot %d of %d", chosen + 1, nsta);
    s_sprite.setTextColor(g[4], TFT_BLACK);
    s_sprite.drawString(sub, cpX + 8, cpY + 52, 1);

    // Optional ICY "on air" line (skipped if we already used it as the
    // big name above -- audioStationDisplayName falls through to ICY
    // second, so an override + ICY both set shows override big, ICY
    // small here).
    const char *icy = audioCurStation();
    if (icy && *icy && stName != icy) {
        String s = icy;
        if (s.length() > 26) s = s.substring(0, 26);
        s_sprite.setTextColor(g[2], TFT_BLACK);
        s_sprite.drawString("On air: " + s, cpX + 8, cpY + 70, 1);
    }

    // Song-title scroll strip is blitted by displayDrawScroll() at the
    // bottom of the card (y=kSongScrollY=150, inside cpY+cpH window).
    s_sprite.drawRect(kSongScrollX - 2, kSongScrollY - 2,
                      kSongScrollW + 4, kSongScrollH + 4, g[11]);

    // ---- Volume column on the right ---------------------------------
    const int vx = cpX + cpW + 2;        // ~212
    const int vyTop = cpY + 4, vyBot = cpY + cpH - 4;
    // ^ placeholder (filled triangle pointing up)
    s_sprite.fillTriangle(vx + 10, vyTop, vx + 3, vyTop + 10, vx + 17, vyTop + 10, g[2]);
    // v placeholder
    s_sprite.fillTriangle(vx + 10, vyBot, vx + 3, vyBot - 10, vx + 17, vyBot - 10, g[2]);
    // Five-segment vertical bar mapping to the audioVolume() 1..5.
    int vol = audioVolume();
    const int segX = vx + 6, segW = 8;
    const int segTop = vyTop + 16, segBot = vyBot - 16;
    const int segTotal = segBot - segTop;
    const int gap = 2, segH = (segTotal - 4 * gap) / 5;
    for (int i = 0; i < 5; i++) {
        // Segments are drawn top-to-bottom but meaning is bottom-to-top
        // (i.e. segment 0 = loudest, segment 4 = quietest). We fill from
        // the bottom up to `vol`.
        int y = segBot - (i + 1) * segH - i * gap;
        uint16_t c = (i < vol) ? g_theme.volumeBar : g[11];
        s_sprite.fillRect(segX, y, segW, segH, c);
    }

    // ---- Station switcher (3 rows, current highlighted) -------------
    const int swY = 174, swH = 42;
    s_sprite.fillRect(0, swY, 240, swH, TFT_BLACK);
    int prev = (chosen - 1 + nsta) % nsta;
    int next = (chosen + 1) % nsta;
    String nmP = audioStationDisplayName(prev); if (nmP.length() > 24) nmP = nmP.substring(0, 24);
    String nmN = audioStationDisplayName(next); if (nmN.length() > 24) nmN = nmN.substring(0, 24);
    // ^ (prev)
    s_sprite.fillTriangle(8, swY + 6, 2, swY + 12, 14, swY + 12, g[4]);
    s_sprite.setTextColor(g[6], TFT_BLACK);
    s_sprite.drawString(nmP, 22, swY + 3, 1);
    // highlighted current
    s_sprite.fillRoundRect(4, swY + 14, 232, 16, 3, bg);
    s_sprite.drawRoundRect(4, swY + 14, 232, 16, 3, orange);
    s_sprite.setTextColor(TFT_YELLOW, bg);
    s_sprite.drawString(stName, 10, swY + 16, 2);
    // v (next)
    s_sprite.fillTriangle(8, swY + 36, 2, swY + 30, 14, swY + 30, g[4]);
    s_sprite.setTextColor(g[6], TFT_BLACK);
    s_sprite.drawString(nmN, 22, swY + 31, 1);

    // ---- Footer: RSSI + bitrate -------------------------------------
    s_sprite.fillRect(0, 218, 240, 22, bg);
    s_sprite.fillRect(0, 217, 240, 1, orange);
    char foot[48];
    snprintf(foot, sizeof(foot), "RSSI %d dBm   BITRATE %ld kbps",
             netRssi(), audioBitrate());
    s_sprite.setTextColor(g[2], bg);
    s_sprite.drawString(foot, 6, 223, 1);

    // Mute unused globals warnings for variables kept for future tweaks.
    (void)g; (void)orange; (void)light;

    blitSprite(s_sprite, 0, 0, DISPLAY_W, DISPLAY_H);
    s_repaint = false;

    // Refresh the song-title ticker too -- the main repaint clears it.
    displayDrawScroll();
}
