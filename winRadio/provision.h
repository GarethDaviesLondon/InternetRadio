// First-time / user-triggered WiFi provisioning via a SoftAP + captive
// portal. The AP is named after PROVISION_AP_SSID (config.h).
//
// Flow: provisionStart() brings up the AP, a DNS responder (so phones
// auto-open the captive page), and a small WebServer that renders the
// last scan + a "Save" form. When the user submits the form, the handler
// calls wifiAddNetwork(), which bumps the saved-network count and lets
// cliWaitForNewNetwork() exit. The orchestrator then calls
// provisionStop() which tears the AP down and returns to STA mode.

#pragma once

#include <Arduino.h>

void provisionStart();
void provisionPoll();       // drive DNS + web server; call from the wait loop
void provisionStop();
bool provisionActive();
String provisionApIp();     // "" if AP not running
