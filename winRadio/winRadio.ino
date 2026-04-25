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
#include "imu.h"

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

// Sticky flag: latched by the portal abort callback, cleared once the
// boot loop reads it. Lets the post-netConnect code distinguish
// "user clicked abort on the portal -> just retry the saved list"
// from "we genuinely walked everything and need setup mode".
static bool s_abortViaPortal = false;

// Abort callback passed to netConnect(): triggers when EITHER the
// right button (V) is held OR the user clicked "Abort current
// attempt" on the background AP portal. bootTick is called here too
// so sleep / reboot / dim / portal HTTP keep working while we're
// trying each saved network.
static bool netAbortOnRightButton() {
    bootTick();
    if (provisionAbortRequested()) {
        provisionClearAbort();
        s_abortViaPortal = true;
        Serial.println("boot: abort requested via AP portal");
        return true;
    }
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
    imuBegin();   // accel + gyro for motion-wake and face-down-pause

    // SD card is optional. If it mounts, /theme.ini and (on first boot)
    // /stations.csv feed the station list; otherwise silently continue.
    storageSdMount();
    // Load the active station list (NVS on later boots, SD CSV or
    // compiled defaults on first boot).
    stationsBegin();

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
        // Distinguish "user clicked abort on portal" from "exhausted
        // the list". The portal-abort case keeps the AP up and just
        // retries netConnect (the user has already reordered or added
        // an entry); the exhausted case drops into setup mode where
        // they can fix things.
        if (s_abortViaPortal) {
            s_abortViaPortal = false;
            displayShowMessage("Aborted.", "Retrying", "saved networks...");
            delay(600);
            continue;
        }
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

    // Boot-time captive-portal probe. If the freshly-joined network
    // is intercepting HTTP, surface the situation on the LCD instead
    // of letting the radio sit silently with audio that won't start.
    if (netConnected()) {
        CaptiveStatus cs = netCheckCaptive();
        Serial.printf("captive: boot probe -> %s\r\n", netCaptiveStatusName(cs));
        if (cs == CAPTIVE_PORTAL) {
            String ssid = netCurrentSsid();
            displayCaptiveOpen(ssid.c_str(), netCaptivePortalUrl());
        }
    }

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

    // Captive-portal background probe. Re-checks every 15 s while a
    // captive screen is up; if the portal lets us through, audio is
    // resumed and the screen returns to Now Playing.
    static unsigned long lastCaptiveProbe = 0;
    if (displayActiveMode() == DM_CAPTIVE &&
        millis() - lastCaptiveProbe > 15000) {
        lastCaptiveProbe = millis();
        if (netCheckCaptive() == CAPTIVE_ONLINE) {
            Serial.println("captive: portal cleared; resuming audio");
            audioStartLast();
            displayCaptiveDismiss();
            displayRequestRepaint();
        }
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

    // System-info screen: any short press exits back to the prior mode.
    // Long-presses (sleep, picker-open) still pass through.
    if (displayActiveMode() == DM_SYS_INFO) {
        switch (ev) {
            case INPUT_SYS_INFO:
                // Long-press Right while in sysinfo -> open the on-device
                // WiFi picker. The picker triggers a fresh scan.
                displayWifiPickerOpen();
                ev = INPUT_NONE;
                displayRequestRepaint();
                break;
            case INPUT_NEXT:
            case INPUT_VOL_UP:
            case INPUT_MODE_TOGGLE:
            case INPUT_PLAY_PAUSE:
                displayModalClose();   // restores prior mode
                ev = INPUT_NONE;
                displayRequestRepaint();
                break;
            default: break;
        }
    }
    // Station picker:
    //   Right short      = cursor forward (next)
    //   Mid short        = cursor backward (prev)
    //   Mid double-click = select
    //   Mid long-press   = select (was: open picker, but already here)
    //   Right long-press = select (was: open sysinfo, ditto)
    //   Left short       = exit (cancel)
    else if (displayActiveMode() == DM_PICKER) {
        switch (ev) {
            case INPUT_VOL_UP:         // right short -- forward
                displayPickerAdvance();
                ev = INPUT_NONE;
                break;
            case INPUT_NEXT:           // mid short -- backward
                displayPickerRetreat();
                ev = INPUT_NONE;
                break;
            case INPUT_PICKER_SELECT:  // mid double-click
            case INPUT_PICKER_OPEN:    // mid long-press (already in picker)
            case INPUT_SYS_INFO:       // right long-press (already in picker)
            {
                int slot = displayPickerSelectedSlot();
                if (slot >= 0) audioSelectStation(slot);
                displayModalClose();
                displayRequestRepaint();
                ev = INPUT_NONE;
                break;
            }
            case INPUT_MODE_TOGGLE:    // left short
                displayModalClose();
                displayRequestRepaint();
                ev = INPUT_NONE;
                break;
            default: break;
        }
    }
    // WiFi picker: Mid short advances cursor, Mid double-click triggers
    // a connect, Left/Right short cancels.
    else if (displayActiveMode() == DM_WIFI_PICKER) {
        switch (ev) {
            case INPUT_NEXT:
                displayWifiPickerAdvance();
                ev = INPUT_NONE;
                break;
            case INPUT_PICKER_SELECT:
            {
                int idx = displayWifiPickerSelected();
                if (idx >= 0) {
                    const char *ssid = displayWifiPickerSsidAt(idx);
                    // Look up stored creds; empty string if none.
                    String pass;
                    int n = wifiNetworkCount();
                    for (int i = 0; i < n; i++) {
                        if (String(ssid).equalsIgnoreCase(wifiNetworkSsid(i))) {
                            pass = wifiNetworkPass(i);
                            break;
                        }
                    }
                    displayWifiConnectShow(ssid);
                    displayDrawMain();   // draw the connecting screen now
                    bool ok = netConnectAdhoc(String(ssid), pass, 12000,
                                              displayWifiConnectTick);
                    if (ok) {
                        // Persist new SSID -> creds. wifiAddNetwork
                        // updates in-place when SSID matches an
                        // existing slot.
                        wifiAddNetwork(String(ssid), pass);
                        // Promote to slot 0: the user explicitly
                        // picked this network NOW, so the next boot
                        // should try it first. Find the slot the
                        // SSID landed in and bump it to the top.
                        for (int i = 0; i < wifiNetworkCount(); i++) {
                            if (String(ssid).equalsIgnoreCase(wifiNetworkSsid(i))) {
                                wifiPromoteNetwork(i);
                                break;
                            }
                        }
                        // Probe for a captive portal before kicking
                        // off audio. If we're behind one, drop into
                        // the captive screen instead of trying to
                        // connect a stream that will just 30x.
                        CaptiveStatus cs = netCheckCaptive();
                        if (cs == CAPTIVE_PORTAL) {
                            displayCaptiveOpen(ssid, netCaptivePortalUrl());
                        } else {
                            audioStartLast();
                        }
                    } else {
                        displayWifiConnectFail(
                            pass.length() ? "Saved password failed."
                                          : "Open auth failed.");
                        displayDrawMain();
                        delay(1500);
                    }
                    displayWifiConnectDone(ok);
                    displayRequestRepaint();
                }
                ev = INPUT_NONE;
                break;
            }
            case INPUT_MODE_TOGGLE:
            case INPUT_VOL_UP:
                displayModalClose();
                displayRequestRepaint();
                ev = INPUT_NONE;
                break;
            default: break;
        }
    }
    // Connect screen is purely informational; eat all short input so a
    // bouncing button doesn't cancel mid-connect.
    else if (displayActiveMode() == DM_WIFI_CONNECT) {
        if (ev != INPUT_SLEEP) ev = INPUT_NONE;
    }
    // Station details: any short press returns to NP. Long-presses
    // (sleep, sysinfo, picker) still pass through.
    else if (displayActiveMode() == DM_STATION_DETAIL) {
        switch (ev) {
            case INPUT_NEXT:
            case INPUT_VOL_UP:
            case INPUT_MODE_TOGGLE:
            case INPUT_PLAY_PAUSE:
            case INPUT_PICKER_SELECT:
                displayModalClose();
                displayRequestRepaint();
                ev = INPUT_NONE;
                break;
            default: break;
        }
    }
    // Captive-portal screen: Left short dismisses (background probe
    // continues retrying every 15 s and will reopen if still blocked).
    // Other inputs pass through so the user can still toggle modes.
    else if (displayActiveMode() == DM_CAPTIVE) {
        if (ev == INPUT_MODE_TOGGLE) {
            displayCaptiveDismiss();
            displayRequestRepaint();
            ev = INPUT_NONE;
        }
    }

    switch (ev) {
        case INPUT_NEXT:        audioNextStation(); displayRequestRepaint(); break;
        case INPUT_PREV:        audioPrevStation(); displayRequestRepaint(); break;
        case INPUT_VOL_UP:      audioSetVolume((audioVolume() % 5) + 1);
                                displayRequestRepaint(); break;
        case INPUT_VOL_DOWN:    audioSetVolume(audioVolume() - 1);
                                displayRequestRepaint(); break;
        case INPUT_MODE_TOGGLE: displayToggleMode(); break;
        case INPUT_PLAY_PAUSE:
            // Left double-click. In Now Playing, open the station-detail
            // screen (URL, ICY, bitrate). In other home modes, fall back
            // to the original play/pause shortcut.
            if (displayActiveMode() == DM_NOW_PLAYING) {
                displayStationDetailOpen();
                displayRequestRepaint();
            } else {
                audioTogglePause();
                displayRequestRepaint();
            }
            break;
        case INPUT_SLEEP:       powerDeepSleep(); break;
        case INPUT_PICKER_OPEN: displayPickerOpen(); displayRequestRepaint(); break;
        case INPUT_SYS_INFO:    displaySysInfoOpen();
                                displayRequestRepaint(); break;
        default: break;
    }

    // IMU: motion wakes the screen; face-down / face-up toggles pause
    // automatically so you can silence the radio by laying it face-
    // down on the table, and it resumes when you pick it up.
    imuLoop();
    if (imuMotionEvent()) displayNoteActivity();
    static bool s_imuPausedByFaceDown = false;
    if (imuFaceDownEvent()) {
        if (!audioIsPaused()) {
            audioTogglePause();
            s_imuPausedByFaceDown = true;
            displayRequestRepaint();
        }
    }
    if (imuFaceUpEvent()) {
        if (s_imuPausedByFaceDown && audioIsPaused()) {
            audioTogglePause();
            displayRequestRepaint();
        }
        s_imuPausedByFaceDown = false;
        displayNoteActivity();
    }

    cliPoll();
    webPoll();

    vTaskDelay(1);
    audioLoop();

    displayBacklightTick();
    if (displayRepaintPending()) displayDrawMain();
}
