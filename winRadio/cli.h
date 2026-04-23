// Serial CLI on USB-CDC (9600 8N1, line endings: CR / LF / CRLF).
// Commands operate against the module APIs (audio, net, storage, power);
// this header only exposes the lifecycle entry points.

#pragma once

#include <Arduino.h>

void cliBegin();            // banner + prompt
void cliFirstRunSetup();    // blocking: prompt for SSID/pass if NVS empty
void cliPoll();             // non-blocking; call from loop()

// Block, polling the serial CLI (and provision portal if active), until
// the saved-network count increases OR cliCancelSetup() is called.
// `tickCb` is invoked once per poll cycle (nullable); the orchestrator
// uses it to drive backlight dim / sleep / reboot while we sit here.
void cliWaitForNewNetwork(void (*tickCb)() = nullptr);

// Set by the `cancel` CLI command -- forces cliWaitForNewNetwork() to
// return immediately without a saved network. The orchestrator will fall
// through to netConnect() (which will just fail again if no creds exist;
// that's the expected UX for "let me back out").
void cliCancelSetup();
