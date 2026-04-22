// Waveshare Internet Radio -- main orchestrator.
// All real work lives in the modules; this file is just setup() + loop()
// glue. See README.md for the module map.

#include <Arduino.h>
#include <Wire.h>

#include "config.h"
#include "storage.h"
#include "net.h"
#include "radio_audio.h"
#include "display.h"
#include "input.h"
#include "power.h"
#include "cli.h"
#include "web.h"
#include "provision.h"

// --- WiFi connect UX ------------------------------------------------------

// Abort callback passed to netConnect(): if the user presses the right
// button mid-attempt we break out and enter setup mode.
static bool netAbortOnRightButton() { return inputRightHeld(); }

// Progress callback: repaint "Connecting to X (slot N of M)" on the LCD.
static void netProgressUi(int slot, int total, const char *ssid) {
    displayShowConnecting(ssid, slot, total, "Hold [V] = setup");
}

// Busy-wait for the configured duration, returning early with `true` if
// the right button (V) gets pressed at any point.
static bool waitOrSetupButton(uint32_t durationMs) {
    uint32_t t0 = millis();
    while (millis() - t0 < durationMs) {
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
    cliWaitForNewNetwork();   // polls cli + provisionPoll; exits when a network is saved
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

    // Buttons.
    switch (inputPoll()) {
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

    if (displayRepaintPending()) displayDrawMain();
}
