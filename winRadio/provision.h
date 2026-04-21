// First-time WiFi provisioning (stub).
//
// Trigger: net.cpp's netConnect() returns false (no creds OR connect
// failed). The orchestrator hands off to provisionStart().
//
// Planned implementation:
//   1. WiFi.mode(WIFI_AP); WiFi.softAP(PROVISION_AP_SSID, PROVISION_AP_PASS)
//   2. Bring up a captive-portal DNS responder (DNSServer answering * to
//      our AP IP) so any HTTP probe redirects to the setup page
//   3. ESPAsyncWebServer with /scan (returns visible SSIDs as JSON) and
//      /save (POST {ssid, pass} -> wifiAddNetwork, then reboot)
//   4. The LCD shows "Connect to WaveRadio-Setup, open http://192.168.4.1"
//   5. After save: WiFi.softAPdisconnect, ESP.restart() so netConnect()
//      runs against the new creds on next boot
//
// Today the module exposes the entry points so the rest of the code is
// already wired; the body is a no-op.

#pragma once

void provisionStart();      // begin AP + captive portal (no-op today)
void provisionPoll();       // service DNS / web while in AP mode
void provisionStop();
bool provisionActive();
