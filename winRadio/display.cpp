#include "display.h"
#include "config.h"
#include "audio.h"
#include "net.h"
#include "power.h"
#include "stations.h"
#include "NotoSansBold15.h"

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
static int              s_graph[14] = {0};
static const String     s_btnLabels[3] = {"P","S","V"};
static bool             s_repaint = false;

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

// --- Begin ---------------------------------------------------------------

void displayBegin() {
    s_bus = new Arduino_ESP32SPI(PIN_LCD_DC, PIN_LCD_CS,
                                 PIN_LCD_SCK, PIN_LCD_MOSI, -1);
    s_gfx = new Arduino_ST7789(s_bus, PIN_LCD_RST, 0, true,
                               DISPLAY_W, DISPLAY_H);
    s_gfx->begin();
    s_gfx->fillScreen(RGB565_BLACK);
    analogWrite(PIN_LCD_BL, 110);

    s_sprite.setColorDepth(16);
    // Push the 115 KB sprite into PSRAM so the Audio library can claim
    // contiguous DMA-capable internal SRAM.
    s_sprite.setPsram(true);
    s_sprite.createSprite(DISPLAY_W, DISPLAY_H);
    s_sprite2.createSprite(230, 16);
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
    if (s_songPosition < -220) s_songPosition = 220;
    s_sprite2.fillSprite(TFT_BLACK);
    s_sprite2.drawString(audioSongPlaying(), s_songPosition, 5);
    blitSprite(s_sprite2, 5, 213, 230, 16);
}

void displayDrawMain() {
    const uint16_t bg     = g_theme.bg;
    const uint16_t light  = g_theme.panelBorder;
    const uint16_t orange = g_theme.orange;
    auto &g = g_theme.grays;

    s_sprite.fillRect(0, 0, 240, 240, bg);

    // station list frame
    s_sprite.fillRect(4, 20, 150, 172, BLACK);
    s_sprite.drawRect(4, 20, 150, 172, light);
    // top-right info frame (RSSI / battery)
    s_sprite.fillRect(160, 20, 74, 60, BLACK);
    s_sprite.drawRect(160, 20, 74, 60, light);
    s_sprite.fillRect(174, 24, 5, 10, TFT_RED);
    s_sprite.fillRect(174, 37, 5, 10, TFT_GREEN);

    // battery
    int batLevel = powerBatteryLevel();
    s_sprite.drawRect(210, 36, 17, 10, TFT_GREEN);
    s_sprite.fillRect(212, 38, batLevel, 6, TFT_GREEN);
    s_sprite.fillRect(227, 39, 2, 4, TFT_GREEN);

    // bitrate frame
    s_sprite.fillRect(160, 176, 74, 16, BLACK);
    s_sprite.drawRect(160, 176, 74, 16, light);

    // volume bar
    int vol = audioVolume();
    s_sprite.fillRoundRect(160, 140, 74, 3, 2, g_theme.volumeBar);
    s_sprite.fillRoundRect(146 + (vol * 15), 137, 14, 8, 2, g[2]);
    s_sprite.fillRoundRect(149 + (vol * 15), 139,  8, 4, 2, g[10]);

    // song title strip frame (text comes from displayDrawScroll)
    s_sprite.fillRect(4, 212, 232, 18, BLACK);
    s_sprite.drawRect(4, 212, 232, 18, light);

    // station list slider rail
    s_sprite.fillRect(149, 20, 5, 172, g[11]);
    int sliderPos = 12;
    s_sprite.fillRect(149, sliderPos + 8, 5, 20, g[2]);
    s_sprite.fillRect(151, sliderPos + 12, 1, 12, g[16]);

    // accent bars
    s_sprite.fillRect(4, 7, 150, 3, orange);
    s_sprite.fillRect(190, 5, 45, 3, orange);
    s_sprite.fillRect(160, 194, 74, 1, orange);
    s_sprite.fillRect(190, 11, 45, 3, g[6]);

    // outer chrome
    s_sprite.drawRect(0, 0, 239, 239, light);
    s_sprite.fillRect(5, 234, 230, 2, g[13]);

    // labels
    s_sprite.setTextColor(g[1], bg);
    s_sprite.drawString(" STATIONS ", 42, 2, 2);
    s_sprite.drawString("WEB",       160, 2, 2);

    // station list
    int chosen = audioCurrentStation();
    int n = stationsCount();
    for (int i = 0; i < n; i++) {
        s_sprite.setTextColor(i == chosen ? TFT_GREEN : TFT_DARKGREEN, TFT_BLACK);
        String label = stationsName(i);
        if (label.length() > 20) label = label.substring(0, 20);
        s_sprite.drawString(label, 10, 26 + (i * 19), 2);
    }

    // brand
    s_sprite.setTextColor(g[0], bg);
    s_sprite.drawString("INTERNET", 160, 86);
    s_sprite.setTextColor(TFT_RED, bg);
    s_sprite.drawString("RADIO", 160, 102);

    s_sprite.setTextColor(g[6], bg);
    s_sprite.drawString("SONG PLAYING", 6, 200, 1);
    s_sprite.drawString("VOLUME", 160, 124, 1);

    // wifi corner block
    s_sprite.setTextColor(g[10], TFT_BLACK);
    s_sprite.drawString("W", 165, 24, 1);
    s_sprite.drawString("I", 165, 34, 1);
    s_sprite.drawString("F", 165, 44, 1);
    s_sprite.drawString("I", 165, 54, 1);

    s_sprite.setTextColor(TFT_GREEN, TFT_BLACK);
    s_sprite.drawString("BITRATE " + String(audioBitrate()), 164, 180, 1);
    s_sprite.drawString("RSSI:" + String(netRssi()),         183, 24, 1);
    s_sprite.drawString(String(powerBatteryVolts()),         183, 37, 1);

    s_sprite.setTextColor(g[11], bg);
    s_sprite.drawString(FIRMWARE_NAME, 122, 200, 1);

    // graph (random sparkline while connected)
    bool live = netConnected();
    for (int i = 0; i < 12; i++) {
        if (live) s_graph[i] = random(1, 5);
        for (int j = 0; j < s_graph[i]; j++)
            s_sprite.fillRect(172 + (i * 5), 71 - j * 4, 4, 3, g[4]);
    }

    // virtual buttons
    s_sprite.setTextColor(g[16], g[5]);
    for (int i = 0; i < 3; i++) {
        s_sprite.fillRoundRect(160 + (i * 26), 152, 22, 18, 4, g[5]);
        s_sprite.drawString(s_btnLabels[i], 166 + (i * 26), 154);
    }

    blitSprite(s_sprite, 0, 0, DISPLAY_W, DISPLAY_H);
    s_repaint = false;

    // Refresh the song-title ticker too -- the main repaint clears it.
    displayDrawScroll();
}
