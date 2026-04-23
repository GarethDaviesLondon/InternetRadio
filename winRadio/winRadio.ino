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

// Progress callback: repaint "Connecting to X (slot N of M)" on the LCD.
static void netProgressUi(int slot, int total, const char *ssid) {
    displayShowConnecting(ssid, slot, total, "Hold [V] = setup");
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

    audioCodecInit();
    audioPlayMorseR();   // dot-dash-dot self-test before Audio lib grabs I2S 0
    audioRestoreSession();   // read last volume + station from NVS

    displayBegin();
    if (storageSdMounted()) displayLoadThemeFromSd();
    displayShowBootSplash("Scanning WiFi...");

    netBegin();
    netScanNow();

    // Show the scan results for ~4 s. During that window the user can
    // press the right button (V) to jump straight into WiFi setup.
    displayShowWifiScan("Hold [V] = setup", -1);
    bool setupRequested = waitOrSetupButton(4000);

    if (setupRequested || !wifiHasNetworks()) {
        runWifiSetup();
    }

    // Try all saved networks in order. If every attempt fails or the user
    // aborts with the right button, fall over into setup mode and retry.
    while (!netConnect(netAbortOnRightButton, netProgressUi)) {
        displayShowMessage("No network", "connected.",
                           "Entering setup...");
        delay(800);
        runWifiSetup();
    }

    audioBegin();
    audioStartLast();    // resumes the remembered station

    webBegin();          // no-op until web CLI lands

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
        case INPUT_NEXT:    audioNextStation(); displayRequestRepaint(); break;
        case INPUT_PREV:    audioPrevStation(); displayRequestRepaint(); break;
        case INPUT_VOL_UP:  audioSetVolume((audioVolume() % 5) + 1);
                            displayRequestRepaint(); break;
        case INPUT_VOL_DOWN:audioSetVolume(audioVolume() - 1);
                            displayRequestRepaint(); break;
        case INPUT_SLEEP:   powerDeepSleep(); break;
        default: break;
    }

    cliPoll();
    webPoll();

    vTaskDelay(1);
    audioLoop();

    displayBacklightTick();
    if (displayRepaintPending()) displayDrawMain();
}
