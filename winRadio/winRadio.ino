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

void setup() {
    // Serial CLI on USB-CDC. Baud is virtualised; PuTTY's setting is cosmetic.
    Serial.begin(9600);
    {
        unsigned long t0 = millis();
        while (!Serial && millis() - t0 < 1500) delay(10);
    }
    cliBegin();

    storageLoadWifiCreds();
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    inputBegin();
    powerBegin();

    audioCodecInit();
    audioPlayMorseR();   // dot-dash-dot self-test before Audio lib grabs I2S 0

    displayBegin();
    displayShowMessage("connecting", "to WI-FI");

    netBegin();
    if (!storageHasWifiCreds()) {
        displayShowMessage("No WiFi creds.",
                           "Connect serial",
                           "@ 9600 8N1",
                           "and type: wifi");
        cliFirstRunSetup();
    }
    if (!netConnect()) {
        // TODO(provision): when provisionStart() is implemented, fall over
        // to the AP/captive-portal flow here instead of just continuing.
        provisionStart();
    }

    audioBegin();
    audioStartDefault();

    webBegin();          // no-op until implemented

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
