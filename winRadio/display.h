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

// Backlight control. Levels are the raw 0..255 PWM duty; the helpers below
// wrap an idle-timeout dim behaviour (full brightness when active, dim
// after N ms of no user input, off on request).
void     displaySetBacklight(uint8_t level);
uint8_t  displayBacklight();
void     displayNoteActivity();          // called by input / web / CLI
void     displayBacklightTick();         // call ~once per loop; applies dim

// Read an ini-style file from SD (key=value per line; '#' and ';' start
// comments; values either as 0xRRGGBB, 0xXXXX RGB565, or "R,G,B" triples).
// Silently no-ops if SD isn't mounted or the file isn't present.
// Recognised keys: bg, orange, panelBg, panelBorder, volumeBar.
bool displayLoadThemeFromSd(const char *path = "/theme.ini");

// Per-frame APIs. drawMain() repaints the whole UI; drawScroll() advances
// the bottom song-title ticker. Cheap to call drawScroll() every ~30 ms.
void displayDrawMain();
void displayDrawScroll();

// Home-screen modes. Short-press on the left button cycles. The "big
// clock" mode hides the now-playing card and station switcher and shows
// a large HH:MM:SS in the centre; the banner + footer stay put.
enum DisplayMode {
    DM_NOW_PLAYING    = 0,
    DM_BIG_CLOCK      = 1,
    DM_SYS_INFO       = 2,   // long-press Right -> battery + WiFi + IP
    DM_PICKER         = 3,   // long-press Mid  -> 3x3 station grid
    DM_WIFI_PICKER    = 4,   // long-press Right while in sysinfo -> SSID grid
    DM_WIFI_CONNECT   = 5,   // showing "connecting to <ssid>..." progress
    DM_STATION_DETAIL = 6,   // Left double in Now Playing -> stream details
};
void        displaySetMode(DisplayMode m);
void        displayToggleMode();          // cycles only between NowPlaying / BigClock
DisplayMode displayActiveMode();

// Modal screens. Open* sets the mode and remembers the prior mode so
// Close* can restore it; Close also clears the modal state.
void displaySysInfoOpen();
void displayStationDetailOpen();           // Left double in NP -> details
void displayPickerOpen();                 // resets cursor to current slot
void displayPickerAdvance();              // cursor forward (wraps)
void displayPickerRetreat();              // cursor backward (wraps)
int  displayPickerSelectedSlot();         // current cursor index
void displayModalClose();                 // restore prior home mode

// WiFi picker (3x3 grid of scanned SSIDs, paged like the station picker).
void displayWifiPickerOpen();             // triggers a scan, sets mode
void displayWifiPickerAdvance();          // advance cursor through SSID list
int  displayWifiPickerSelected();         // index into the scan-result list, -1 if empty
int  displayWifiPickerSsidCount();
const char *displayWifiPickerSsidAt(int i);

// "Connecting to <ssid>..." progress screen. Caller fills the elapsed
// counter via displayWifiConnectTick(); displayWifiConnectShow() opens
// the screen with the target SSID.
void displayWifiConnectShow(const char *ssid);
void displayWifiConnectTick(uint32_t elapsedMs);
void displayWifiConnectFail(const char *reason);   // briefly shows error
void displayWifiConnectDone(bool ok);              // restores prior mode if ok

// Lightweight: ask for a full repaint at the next loop tick.
void displayRequestRepaint();
bool displayRepaintPending();

// Boot-time message (used for "No WiFi creds" prompt etc.).
void displayShowMessage(const char *line1,
                        const char *line2 = nullptr,
                        const char *line3 = nullptr,
                        const char *line4 = nullptr);

// Full-screen boot splash with the ON8CIT logo + branding + a status line.
// Use it early in setup() so the user has something to look at while WiFi
// connects; subsequent WiFi screens render the same branding header.
void displayShowBootSplash(const char *status = nullptr);

// Boot-time WiFi screens. Read the scan results from net.h (call
// netScanNow() first) and display them. Footer prints in yellow at the
// bottom -- typically "Hold [V] for setup".
void displayShowWifiScan(const char *footer = nullptr, int highlightIdx = -1);
void displayShowConnecting(const char *ssid, int slot, int total,
                           const char *footer = nullptr);
// Compact view shown after the 10 s boot splash while netConnect() is
// still iterating saved slots: small logo, scan list, commentary row.
void displayShowCompactConnect(const char *ssid, int slot, int total,
                               const char *footer = nullptr);
// Shown while the AP provisioning portal is active.
void displayShowSetupMode(const char *apSsid, const char *apIp);
