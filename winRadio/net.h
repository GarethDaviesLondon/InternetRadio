// Network: WiFi STA connect/reconnect, RSSI snapshot, hostname / mDNS,
// and AP-fallback / captive-portal provisioning hook.
//
// netConnect() is the main entry. It tries the stored credentials and, if
// they fail (or none are stored), falls through to the provision module.

#pragma once

#include <Arduino.h>

void  netBegin();                 // sets WiFi mode + hostname; non-blocking
bool  netConnect();               // blocking connect using stored creds
void  netReconnect();              // disconnect + reconnect using stored creds
bool  netConnected();
String netLocalIp();
int   netRssi();
const char *netHostname();

// Discovery on the local network. Stub today; ESP-IDF mDNS once enabled.
void  netStartMdns(const char *hostname); // future: announce _http._tcp etc.
void  netStopMdns();

// Future: a tiny LAN-broadcast / SSDP heartbeat so a desktop helper can
// list radios on the LAN without mDNS. Not implemented yet.
void  netBroadcastPresence();
