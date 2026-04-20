#pragma once

#include <Arduino.h>

// NVS-backed WiFi credential storage
bool        loadWifiCreds();     // reads ssid/pass from NVS; returns true if set
bool        hasWifiCreds();
const char *getWifiSsid();
const char *getWifiPassword();

// Serial CLI (9600 8N1, UART0)
void cliBegin();            // print banner + prompt
void cliFirstRunSetup();    // blocking: prompt for creds if NVS is empty
void cliPoll();             // call from loop(); non-blocking
