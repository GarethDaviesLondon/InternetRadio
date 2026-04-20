// Serial CLI + NVS-backed WiFi credential storage.
// Runs on Serial at 9600 8N1 (configured in winRadio.ino).
// Commands: help, wifi, wifi show, wifi clear, status, reboot.

#include <Preferences.h>
#include <WiFi.h>

static Preferences g_prefs;
static String g_ssid = "";
static String g_pass = "";
static String g_cliBuf = "";

// --- credential storage ----------------------------------------------------

bool loadWifiCreds() {
  g_prefs.begin("wifi", true);
  g_ssid = g_prefs.getString("ssid", "");
  g_pass = g_prefs.getString("pass", "");
  g_prefs.end();
  return g_ssid.length() > 0;
}

static void saveWifiCreds(const String &s, const String &p) {
  g_prefs.begin("wifi", false);
  g_prefs.putString("ssid", s);
  g_prefs.putString("pass", p);
  g_prefs.end();
  g_ssid = s;
  g_pass = p;
}

static void clearWifiCreds() {
  g_prefs.begin("wifi", false);
  g_prefs.remove("ssid");
  g_prefs.remove("pass");
  g_prefs.end();
  g_ssid = "";
  g_pass = "";
}

bool hasWifiCreds()           { return g_ssid.length() > 0; }
const char *getWifiSsid()     { return g_ssid.c_str(); }
const char *getWifiPassword() { return g_pass.c_str(); }

// --- line editing ----------------------------------------------------------

static String cliReadLineBlocking(const char *prompt, bool mask) {
  Serial.print(prompt);
  Serial.flush();
  String line = "";
  while (true) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == '\r') continue;
      if (c == '\n') { Serial.println(); return line; }
      if (c == 8 || c == 127) {
        if (line.length()) {
          line.remove(line.length() - 1);
          Serial.print("\b \b");
        }
        continue;
      }
      if (c < 0x20) continue;
      line += c;
      Serial.print(mask ? '*' : c);
    }
    delay(1);
  }
}

static void cliPrintHelp() {
  Serial.println();
  Serial.println(F("=== Waveshare Internet Radio CLI ==="));
  Serial.println(F("  help          Show this help"));
  Serial.println(F("  wifi          Enter new SSID and password"));
  Serial.println(F("  wifi show     Show stored SSID"));
  Serial.println(F("  wifi clear    Erase stored credentials"));
  Serial.println(F("  status        Show radio status"));
  Serial.println(F("  reboot        Restart the device"));
  Serial.println();
}

static void cliPromptForWifi() {
  String s = cliReadLineBlocking("SSID: ", false);
  s.trim();
  if (s.length() == 0) {
    Serial.println(F("Aborted (empty SSID)."));
    return;
  }
  String p = cliReadLineBlocking("Password: ", true);
  saveWifiCreds(s, p);
  Serial.println(F("Saved to NVS. Type 'reboot' to apply."));
}

// --- status ----------------------------------------------------------------

// Globals from winRadio.ino that we surface via `status`.
extern int chosen;
extern int volume;
extern long bitrate;
extern float voltage;

static void cliPrintStatus() {
  Serial.print(F("WiFi    : "));
  Serial.println(WiFi.status() == WL_CONNECTED ? "connected" : "disconnected");
  Serial.print(F("SSID    : "));
  Serial.println(g_ssid.length() ? g_ssid : String("<unset>"));
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("IP      : ")); Serial.println(WiFi.localIP());
    Serial.print(F("RSSI    : ")); Serial.print(WiFi.RSSI()); Serial.println(F(" dBm"));
  }
  Serial.print(F("Station : ")); Serial.println(chosen);
  Serial.print(F("Volume  : ")); Serial.println(volume);
  Serial.print(F("Bitrate : ")); Serial.print(bitrate); Serial.println(F(" kbps"));
  Serial.print(F("Battery : ")); Serial.print(voltage); Serial.println(F(" V"));
}

// --- command dispatch ------------------------------------------------------

static void cliDispatch(const String &raw) {
  String cmd = raw;
  cmd.trim();
  if (cmd.length() == 0) return;

  if (cmd.equalsIgnoreCase("help") || cmd == "?") {
    cliPrintHelp();
  } else if (cmd.equalsIgnoreCase("wifi")) {
    cliPromptForWifi();
  } else if (cmd.equalsIgnoreCase("wifi show")) {
    if (g_ssid.length() == 0) Serial.println(F("No SSID stored."));
    else { Serial.print(F("SSID: ")); Serial.println(g_ssid); }
  } else if (cmd.equalsIgnoreCase("wifi clear")) {
    clearWifiCreds();
    Serial.println(F("Credentials cleared."));
  } else if (cmd.equalsIgnoreCase("status")) {
    cliPrintStatus();
  } else if (cmd.equalsIgnoreCase("reboot")) {
    Serial.println(F("Rebooting..."));
    delay(100);
    ESP.restart();
  } else {
    Serial.print(F("Unknown command: '"));
    Serial.print(cmd);
    Serial.println(F("'. Type 'help'."));
  }
  Serial.print(F("> "));
}

// --- public entry points ---------------------------------------------------

void cliBegin() {
  Serial.println();
  Serial.println(F("Waveshare Internet Radio -- serial CLI ready"));
  Serial.println(F("Type 'help' for commands."));
  Serial.print(F("> "));
}

void cliFirstRunSetup() {
  if (hasWifiCreds()) return;
  Serial.println();
  Serial.println(F("No WiFi credentials in NVS."));
  Serial.println(F("Enter them now via serial (9600 8N1)."));
  while (!hasWifiCreds()) {
    cliPromptForWifi();
  }
}

void cliPoll() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      Serial.println();
      String line = g_cliBuf;
      g_cliBuf = "";
      cliDispatch(line);
    } else if (c == 8 || c == 127) {
      if (g_cliBuf.length()) {
        g_cliBuf.remove(g_cliBuf.length() - 1);
        Serial.print("\b \b");
      }
    } else if (c >= 0x20) {
      g_cliBuf += c;
      Serial.print(c);
    }
  }
}
