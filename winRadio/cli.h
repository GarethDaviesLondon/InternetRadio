// Serial CLI on USB-CDC (9600 8N1, line endings: CR / LF / CRLF).
// Commands operate against the module APIs (audio, net, storage, power);
// this header only exposes the lifecycle entry points.

#pragma once

#include <Arduino.h>

void cliBegin();            // banner + prompt
void cliFirstRunSetup();    // blocking: prompt for SSID/pass if NVS empty
void cliPoll();             // non-blocking; call from loop()
