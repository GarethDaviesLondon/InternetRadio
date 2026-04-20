// Display: ST7789 init, off-screen sprites, draw routines.
//
// All rendering reads through the audio / net / power / stations module
// accessors -- no direct global state coupling. Themes are a one-indirection
// hook: replace g_theme to restyle.

#pragma once

#include <Arduino.h>

struct Theme {
    // Generated palette (gradient of grays), populated by displayBegin().
    uint16_t grays[18];
    // Accent colours.
    uint16_t orange;
    uint16_t panelBg;     // dark inside frames
    uint16_t panelBorder; // light frame stroke colour (= grays[12])
    uint16_t bg;          // outer background (= grays[16])
    uint16_t volumeBar;   // yellow rail
};

extern Theme g_theme;

void displayBegin();        // panel up, sprites allocated, font loaded, palette built
void displaySetTheme(const Theme &t);

// Per-frame APIs. drawMain() repaints the whole UI; drawScroll() advances
// the bottom song-title ticker. Cheap to call drawScroll() every ~30 ms.
void displayDrawMain();
void displayDrawScroll();

// Lightweight: ask for a full repaint at the next loop tick.
void displayRequestRepaint();
bool displayRepaintPending();

// Boot-time message (used for "No WiFi creds" prompt etc.).
void displayShowMessage(const char *line1,
                        const char *line2 = nullptr,
                        const char *line3 = nullptr,
                        const char *line4 = nullptr);
