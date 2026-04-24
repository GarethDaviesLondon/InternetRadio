// Waveshare Internet Radio -- main orchestrator.
// All real work lives in the modules; this file is just setup() + loop()
// glue. See README.md for the module map.

#include <Arduino.h>
#include <Wire.h>

#include "config.h"
#include "storage.h"
#include "net.h"
#include "radio_audio.h"
#include "stations.h"
#include "display.h"
#include "input.h"
#include "power.h"
#include "cli.h"
#include "web.h"
#include "provision.h"
#include "clock.h"

// --- WiFi connect UX ------------------------------------------------------

// bootTick: runs once per iteration of every blocking wait in setup().
// Handles the things the user expects to always work, even before we've
// joined a network: backlight dim, L+R reboot combo, clean Left-press
// deep sleep, and the serial CLI (so the user can type `sleep`, `reboot`,
// `wifi add`, etc. during any boot-time blocking wait). Called from every
// setup()-phase busy loop.
static void bootTick() {
    displayBacklightTick();
    cliPoll();
    provisionPoll();   // services the background AP portal if it's running
    if (inputRebootCombo(3000)) {
        displayShowMessage("Rebooting...");
        delay(500);
        ESP.restart();
    }
    InputEvent ev = inputPoll();
    if (ev != INPUT_NONE) displayNoteActivity();
    if (ev == INPUT_SLEEP) powerDeepSleep();   // doesn't return
    // NEXT / VOL events are ignored during boot: audio isn't running yet.

    // Edge-triggered button diagnostic. Silent while nothing changes;
    // prints one line each time any of L/M/R transitions. Lets the user
    // confirm the GPIOs are actually seeing presses without flooding the
    // serial log at idle.
    static uint8_t lastMask = 0xff;
    uint8_t mask = (inputLeftHeld()  ? 1 : 0)
                 | (inputMidHeld()   ? 2 : 0)
                 | (inputRightHeld() ? 4 : 0);
    if (mask != lastMask) {
        lastMask = mask;
        Serial.printf("boot: L=%d M=%d R=%d\r\n",
                      (mask & 1) ? 1 : 0,
                      (mask & 2) ? 1 : 0,
                      (mask & 4) ? 1 : 0);
    }
}

// Abort callback passed to netConnect(): the right button (V) triggers
// WiFi-setup mode mid-attempt. bootTick is called here too so sleep /
// reboot / dim keep working while we're trying each saved network.
static bool netAbortOnRightButton() {
    bootTick();
    return inputRightHeld();
}

// When did we start showing the boot splash? The progress callback uses
// it to pick between the big splash (first ~10 s) and the compact scan +
// commentary view once the splash has run its course.
static uint32_t s_bootSplashStartMs = 0;
constexpr uint32_t kBootSplashMs = 10000;

// Progress callback: repaint the appropriate boot-time view as each saved
// slot is tried. While the splash window is still open the commentary
// replaces the splash's status line; afterwards we switch to the compact
// logo + scan + commentary layout.
static void netProgressUi(int slot, int total, const char *ssid) {
    if (millis() - s_bootSplashStartMs < kBootSplashMs) {
        String line = String("Trying ") + slot + "/" + total + ": " + (ssid ? ssid : "");
        displayShowBootSplash(line.c_str());
    } else {
        displayShowCompactConnect(ssid, slot, total, "Hold [V] = setup");
    }
}

// Busy-wait for the configured duration, returning early with `true` if
// the right button (V) gets pressed at any point.
static bool waitOrSetupButton(uint32_t durationMs) {
    uint32_t t0 = millis();
    while (millis() - t0 < durationMs) {
        bootTick();
        if (inputRightHeld()) return true;
        delay(20);
    }
    return false;
}

// Block in setup-mode until at least one new saved network appears. The
// AP/captive-portal implementation in commit 3 will plug in alongside the
// CLI path so the same "a network was added" exit covers both routes.
static void runWifiSetup() {
    provisionStart();
    String apIp = provisionApIp();
    displayShowSetupMode(PROVISION_AP_SSID,
                         apIp.length() ? apIp.c_str() : "192.168.4.1");
    cliWaitForNewNetwork(bootTick);  // keeps sleep / reboot / dim alive
    provisionStop();
}

void setup() {
    // Serial CLI on USB-CDC. Baud is virtualised; PuTTY's setting is cosmetic.
    Serial.begin(9600);
    {
        unsigned long t0 = millis();
        while (!Serial && millis() - t0 < 1500) delay(10);
    }
    cliBegin();

    wifiLoadNetworks();
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    inputBegin();
    powerBegin();

    // SD card is optional. If it mounts, use /theme.ini and /stations.csv
    // to override the compiled defaults; otherwise silently continue.
    if (storageSdMount()) {
        stationsLoadFromSd();
    }
    // NVS per-slot overrides (set via the web modal or CLI 'station edit')
    // are applied last so they win over SD / preset name+URL.
    stationsApplyOverrides();

    audioCodecInit();
    audioPlayMorseR();   // dot-dash-dot self-test before Audio lib grabs I2S 0
    audioRestoreSession();   // read last volume + station from NVS

    displayBegin();
    if (storageSdMounted()) displayLoadThemeFromSd();

    // 10 s boot splash -- always held for the full window so the user
    // has something to look at even if connect happens instantly. The
    // splash status updates as each slot is tried (via netProgressUi).
    s_bootSplashStartMs = millis();
    displayShowBootSplash("Starting up...");

    netBegin();
    netScanNow();
    displayShowBootSplash("Scan done. Connecting...");

    // Quick right-button check: setup-request during the splash still
    // works so a user who knows they need to reconfigure doesn't have
    // to wait the 10 s out.
    bool setupRequested = false;
    uint32_t t0 = millis();
    while (!setupRequested && millis() - t0 < 500) {
        if (inputRightHeld()) { setupRequested = true; break; }
        delay(20);
    }
    if (setupRequested || !wifiHasNetworks()) {
        runWifiSetup();
    }

    // Background AP: while we try each saved network, the provisioning
    // portal is reachable too, so the user can jump straight to setup
    // without waiting out every 8 s connect timeout. Save via the portal
    // auto-reboots, so there's no reconcile logic here -- the reboot
    // naturally resumes with the newly-saved network in the list.
    provisionStartBackground();
    while (!netConnect(netAbortOnRightButton, netProgressUi)) {
        displayShowMessage("No network", "connected.",
                           "Entering setup...");
        delay(800);
        // Setup mode uses AP-only (tears down the idle STA) for focus.
        provisionStop();
        runWifiSetup();
        provisionStartBackground();   // background AP up again for next loop
    }
    provisionStop();   // associated: AP no longer needed

    // If we joined inside the 10 s splash window, stay on the splash
    // for the remainder so the boot brand gets its full showing.
    displayShowBootSplash("Connected. Starting audio...");
    while (millis() - s_bootSplashStartMs < kBootSplashMs) {
        bootTick();
        delay(50);
    }

    clockBegin();        // kicks off SNTP now that STA is up

    audioBegin();
    audioStartLast();    // resumes the remembered station

    webBegin();          // main-mode web UI (uses the STA interface)

    displayRequestRepaint();
}

void loop() {
    static unsigned long lastSlow  = 0;
    static unsigned long lastSlide = 0;

    // Slow-tick state refresh: ~4 Hz. Battery, RSSI, repaint trigger.
    if (millis() - lastSlow > 240) {
        lastSlow = millis();
        powerSampleBattery();
        displayRequestRepaint();
    }

    // Smooth song-title scroll: ~33 Hz.
    if (millis() - lastSlide > 30) {
        lastSlide = millis();
        displayDrawScroll();
    }

    // Soft-reboot combo: Left + Right held for 3 s. Needed because the
    // radio has an internal battery, so a power-cycle isn't instant.
    if (inputRebootCombo(3000)) {
        displayShowMessage("Rebooting...");
        delay(500);
        ESP.restart();
    }

    // Buttons. Any user event counts as activity for the backlight dimmer.
    InputEvent ev = inputPoll();
    if (ev != INPUT_NONE) displayNoteActivity();
    switch (ev) {
        case INPUT_NEXT:        audioNextStation(); displayRequestRepaint(); break;
        case INPUT_PREV:        audioPrevStation(); displayRequestRepaint(); break;
        case INPUT_VOL_UP:      audioSetVolume((audioVolume() % 5) + 1);
                                displayRequestRepaint(); break;
        case INPUT_VOL_DOWN:    audioSetVolume(audioVolume() - 1);
                                displayRequestRepaint(); break;
        case INPUT_MODE_TOGGLE: displayToggleMode(); break;
        case INPUT_PLAY_PAUSE:  audioTogglePause(); displayRequestRepaint(); break;
        case INPUT_SLEEP:       powerDeepSleep(); break;
        default: break;
    }

    cliPoll();
    webPoll();

    vTaskDelay(1);
    audioLoop();

    displayBacklightTick();
    if (displayRepaintPending()) displayDrawMain();
}
