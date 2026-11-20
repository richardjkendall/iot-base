/*
 * ESP8266 IoT Bootstrap Template
 *
 * This is a minimal bootstrap firmware for ESP8266 devices (ESP-01 compatible).
 *
 * On first boot (no saved config):
 * 1. Creates WiFi access point "ESP-SETUP-[MAC]" (password: "configure")
 * 2. Serves configuration portal at 192.168.4.1
 * 3. User configures WiFi credentials and API server
 * 4. Saves config and reboots
 *
 * On subsequent boots:
 * 1. Connects to configured WiFi
 * 2. Contacts API server (GET /update) with the standard x-ESP8266-* headers
 * 3. If the server sends a firmware image, installs it and reboots into it
 * 4. New firmware takes over
 *
 * If WiFi can't be reached at boot, the configuration portal is started
 * (pre-filled with the saved settings). If nobody uses it for
 * PORTAL_IDLE_TIMEOUT it reboots and tries WiFi again, so a temporary
 * router outage recovers by itself.
 *
 * Config contract for application firmware: /config.json on LittleFS
 *   { "wifi_ssid": "...", "wifi_password": "...",
 *     "api_server": "host-or-ip", "api_port": 80 }
 * App firmware must be built with the same flash layout (eesz=1M64) so it
 * finds the same LittleFS partition.
 *
 * Factory reset: `make factory` (erases all flash, including the config).
 * A normal reflash keeps the config.
 */

#include <ESP8266WiFi.h>
#include <ESP8266httpUpdate.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// Configuration
#define FIRMWARE_VERSION      "iot-base-1.1.0"
#define DEBUG                 1
#define MAX_WIFI_WAIT         20       // seconds per connection attempt
#define MAX_RECONNECT_FAILS   15       // failed reconnects in loop() before rebooting
#define CONFIG_FILE           "/config.json"
#define AP_SSID_PREFIX        "ESP-SETUP-"
#define AP_PASSWORD           "configure"
#define DNS_PORT              53
#define UPDATE_CHECK_INTERVAL 60000UL  // check for updates every 60 seconds
#define PORTAL_IDLE_TIMEOUT   300000UL // fallback portal reboots after 5 min idle

// Global variables
String wifiSsid = "";
String wifiPassword = "";
String apiServer = "";
int apiPort = 80;
bool configLoaded = false;
bool portalMode = false;
unsigned long lastUpdateCheck = 0;
unsigned long lastPortalActivity = 0;
int reconnectFailures = 0;
String networkOptions = "";   // cached <option> list from the last WiFi scan

// Global clients
WiFiClient wifiClient;
ESP8266WebServer server(80);
DNSServer dnsServer;

// ============================================================================
// SERIAL LOGGING FUNCTIONS
// ============================================================================

void writeDebug(String module, String message) {
  Serial.print("$DEBUG,");
  Serial.print(module);
  Serial.print(",");
  Serial.println(message);
}

void writeStatus(String module, String message) {
  Serial.print("$INFO,");
  Serial.print(module);
  Serial.print(",");
  Serial.println(message);
}

void writeError(String module, String message) {
  Serial.print("$ERROR,");
  Serial.print(module);
  Serial.print(",");
  Serial.println(message);
}

// ============================================================================
// HELPERS
// ============================================================================

String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;";  break;
      default:   out += c;
    }
  }
  return out;
}

// ============================================================================
// CONFIGURATION STORAGE (LittleFS)
// ============================================================================

bool loadConfig() {
  if (!LittleFS.begin()) {
    writeError("fs", "Failed to mount LittleFS");
    return false;
  }

  if (!LittleFS.exists(CONFIG_FILE)) {
    writeStatus("config", "No config file found");
    return false;
  }

  File file = LittleFS.open(CONFIG_FILE, "r");
  if (!file) {
    writeError("config", "Failed to open config file");
    return false;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    writeError("config", "Failed to parse config JSON");
    return false;
  }

  wifiSsid = doc["wifi_ssid"] | "";
  wifiPassword = doc["wifi_password"] | "";
  apiServer = doc["api_server"] | "";
  apiPort = doc["api_port"] | 80;

  if (wifiSsid.length() == 0 || apiServer.length() == 0) {
    writeError("config", "Config is missing wifi_ssid or api_server");
    return false;
  }

  writeStatus("config", "Configuration loaded successfully");
  if (DEBUG) {
    writeDebug("wifi_ssid", wifiSsid);
    writeDebug("api_server", apiServer + ":" + String(apiPort));
  }

  return true;
}

bool saveConfig() {
  if (!LittleFS.begin()) {
    writeError("fs", "Failed to mount LittleFS");
    return false;
  }

  File file = LittleFS.open(CONFIG_FILE, "w");
  if (!file) {
    writeError("config", "Failed to create config file");
    return false;
  }

  JsonDocument doc;
  doc["wifi_ssid"] = wifiSsid;
  doc["wifi_password"] = wifiPassword;
  doc["api_server"] = apiServer;
  doc["api_port"] = apiPort;

  if (serializeJson(doc, file) == 0) {
    writeError("config", "Failed to write config");
    file.close();
    return false;
  }

  file.close();
  writeStatus("config", "Configuration saved successfully");
  return true;
}

// ============================================================================
// WIFI CONNECTION
// ============================================================================

bool connectToWiFi() {
  int loopCount = 0;
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid, wifiPassword);   // empty password = open network

  writeStatus("wifi", "Connecting to " + wifiSsid);

  while (WiFi.status() != WL_CONNECTED && loopCount < MAX_WIFI_WAIT) {
    delay(1000);
    writeStatus("wifi", "Still connecting...");
    loopCount++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    writeStatus("wifi", "Connected successfully");
    writeStatus("ip", WiFi.localIP().toString());
    return true;
  } else {
    writeError("wifi", "Connection timed out");
    return false;
  }
}

// ============================================================================
// OTA UPDATE
// ============================================================================

void checkForUpdates() {
  String url = "http://" + apiServer + ":" + String(apiPort) + "/update";

  writeStatus("update", "Checking for updates at " + url);

  // FIRMWARE_VERSION is sent to the server as the x-ESP8266-version header
  t_httpUpdate_return ret = ESPhttpUpdate.update(wifiClient, url, FIRMWARE_VERSION);

  switch(ret) {
    case HTTP_UPDATE_FAILED:
      writeError("update", "Update failed (" + String(ESPhttpUpdate.getLastError()) + "): " +
                 ESPhttpUpdate.getLastErrorString());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      writeStatus("update", "No updates available");
      break;
    case HTTP_UPDATE_OK:
      writeStatus("update", "Update successful - rebooting");
      break;
  }
}

// ============================================================================
// CONFIGURATION PORTAL (WEB SERVER)
// ============================================================================

const char* HTML_HEAD =
  "<!DOCTYPE html><html><head>"
  "<meta name='viewport' content='width=device-width, initial-scale=1'>"
  "<style>"
  "body{font-family:Arial;margin:20px;background:#f0f0f0}"
  ".container{max-width:500px;margin:auto;background:white;padding:20px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1)}"
  "h1{color:#333;text-align:center}"
  "label{display:block;margin-top:15px;color:#555}"
  "input,select{width:100%;padding:8px;margin-top:5px;border:1px solid #ddd;border-radius:4px;box-sizing:border-box}"
  "button{width:100%;padding:12px;margin-top:20px;background:#007bff;color:white;border:none;border-radius:4px;cursor:pointer;font-size:16px}"
  "button:hover{background:#0056b3}"
  ".info{background:#e7f3ff;padding:10px;border-radius:4px;margin-bottom:20px;font-size:14px}"
  ".hint{font-size:12px;color:#888}"
  "</style></head><body>";

const char* HTML_FOOTER = "</body></html>";

// Scans once and caches the result: scanning blocks for ~2s and drops AP
// traffic, so we don't want every captive-portal probe to trigger one.
void scanNetworks() {
  writeStatus("portal", "Scanning for WiFi networks");
  int n = WiFi.scanNetworks();
  networkOptions = "";
  bool savedSsidFound = false;

  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;   // hidden network
    bool selected = (ssid == wifiSsid);
    if (selected) savedSsidFound = true;
    networkOptions += "<option value='" + htmlEscape(ssid) + "'" + (selected ? " selected" : "") + ">" +
                      htmlEscape(ssid) + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }
  WiFi.scanDelete();

  // Saved SSID not visible (or hidden): preselect "custom" so it's pre-filled
  bool useCustom = wifiSsid.length() > 0 && !savedSsidFound;
  networkOptions += String("<option value='custom'") + (useCustom ? " selected" : "") + ">Custom SSID...</option>";

  writeStatus("portal", "Found " + String(n) + " networks");
}

void handleRoot() {
  lastPortalActivity = millis();

  if (server.hasArg("rescan")) {
    scanNetworks();
  }

  String macAddress = WiFi.macAddress();
  bool customSelected = networkOptions.indexOf("value='custom' selected") >= 0;

  String html = HTML_HEAD;
  html += "<div class='container'>";
  html += "<h1>ESP8266 Setup</h1>";
  html += "<div class='info'>Configure your device to connect to WiFi and your API server.<br>";
  html += "<strong>Device MAC:</strong> " + macAddress + "<br>";
  html += "<strong>Firmware:</strong> " FIRMWARE_VERSION;
  if (configLoaded) {
    html += "<br><strong>Note:</strong> could not connect to <em>" + htmlEscape(wifiSsid) + "</em>. Check the settings below.";
  }
  html += "</div>";
  html += "<form action='/save' method='POST'>";

  // WiFi SSID dropdown with cached scan results
  html += "<label>WiFi Network: <a class='hint' href='/?rescan=1'>rescan</a></label>";
  html += "<select name='ssid' id='ssid'>" + networkOptions + "</select>";

  html += "<input type='text' name='custom_ssid' id='custom_ssid' placeholder='Enter custom SSID' value='" +
          (customSelected ? htmlEscape(wifiSsid) : String("")) + "' style='display:" +
          (customSelected ? "block" : "none") + ";margin-top:10px'" + (customSelected ? " required" : "") + ">";

  html += "<label>WiFi Password:</label>";
  html += "<input type='password' name='password' id='password'>";
  html += configLoaded
          ? "<div class='hint'>Leave blank to keep the saved password (for the same network).</div>"
          : "<div class='hint'>Leave blank for an open network.</div>";
  html += "<label style='font-size:14px;margin-top:8px'><input type='checkbox' id='showPassword' style='width:auto;margin-right:5px'> Show password</label>";

  html += "<label>API Server (IP or hostname):</label>";
  html += "<input type='text' name='api_server' placeholder='e.g., 192.168.1.100 or api.example.com' value='" +
          htmlEscape(apiServer) + "' required>";

  html += "<label>API Port:</label>";
  html += "<input type='number' name='api_port' min='1' max='65535' value='" + String(apiPort) + "' required>";

  html += "<button type='submit'>Save & Reboot</button>";
  html += "</form>";

  html += "<script>";
  html += "document.getElementById('ssid').onchange = function() {";
  html += "  var custom = document.getElementById('custom_ssid');";
  html += "  if(this.value === 'custom') { custom.style.display='block'; custom.required=true; }";
  html += "  else { custom.style.display='none'; custom.required=false; }";
  html += "};";
  html += "document.getElementById('showPassword').onchange = function() {";
  html += "  var passwordField = document.getElementById('password');";
  html += "  passwordField.type = this.checked ? 'text' : 'password';";
  html += "};";
  html += "</script>";

  html += "</div>";
  html += HTML_FOOTER;

  server.send(200, "text/html", html);
}

void sendError(int code, const String& message) {
  String html = HTML_HEAD;
  html += "<div class='container'>";
  html += "<h1>Error</h1>";
  html += "<p>" + htmlEscape(message) + "</p>";
  html += "<a href='/'>Go Back</a>";
  html += "</div>";
  html += HTML_FOOTER;
  server.send(code, "text/html", html);
}

void handleSave() {
  lastPortalActivity = millis();

  String selectedSsid = server.arg("ssid");
  String newSsid = (selectedSsid == "custom") ? server.arg("custom_ssid") : selectedSsid;
  String newServer = server.arg("api_server");
  long newPort = server.arg("api_port").toInt();
  newSsid.trim();
  newServer.trim();

  if (newSsid.length() == 0) {
    sendError(400, "WiFi network name is required.");
    return;
  }
  if (newServer.length() == 0) {
    sendError(400, "API server is required.");
    return;
  }
  if (newPort < 1 || newPort > 65535) {
    sendError(400, "API port must be between 1 and 65535.");
    return;
  }

  String newPassword = server.arg("password");
  // Blank password on the fallback portal with the same SSID = keep the saved one
  if (!(newPassword.length() == 0 && configLoaded && newSsid == wifiSsid)) {
    wifiPassword = newPassword;
  }
  wifiSsid = newSsid;
  apiServer = newServer;
  apiPort = (int)newPort;

  if (saveConfig()) {
    String html = HTML_HEAD;
    html += "<div class='container'>";
    html += "<h1>Configuration Saved!</h1>";
    html += "<div class='info'>Device will reboot and connect to your WiFi network.</div>";
    html += "<p>SSID: " + htmlEscape(wifiSsid) + "</p>";
    html += "<p>API Server: " + htmlEscape(apiServer) + ":" + String(apiPort) + "</p>";
    html += "</div>";
    html += HTML_FOOTER;

    server.send(200, "text/html", html);

    delay(2000);
    ESP.restart();
  } else {
    sendError(500, "Failed to save configuration. Please try again.");
  }
}

void handleNotFound() {
  // Redirect all unknown requests to root (captive portal behavior)
  server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
  server.send(302, "text/plain", "");
}

void startConfigPortal() {
  portalMode = true;
  lastPortalActivity = millis();

  String macAddress = WiFi.macAddress();
  macAddress.replace(":", "");
  String apName = AP_SSID_PREFIX + macAddress.substring(6);

  // AP+STA so we can still scan for networks while the AP is up
  WiFi.disconnect();
  WiFi.mode(WIFI_AP_STA);
  scanNetworks();
  WiFi.softAP(apName.c_str(), AP_PASSWORD);

  writeStatus("ap", "Access Point started");
  writeStatus("ap", "SSID: " + apName);
  writeStatus("ap", "Password: " + String(AP_PASSWORD));
  writeStatus("ap", "IP: " + WiFi.softAPIP().toString());

  // Setup DNS server for captive portal
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  // Setup web server routes
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);
  server.begin();

  writeStatus("portal", "Configuration portal started at http://" + WiFi.softAPIP().toString());
}

// ============================================================================
// SETUP & LOOP
// ============================================================================

void setup() {
  // Initialize serial
  Serial.begin(115200);
  Serial.println();
  writeStatus("boot", "ESP8266 Bootstrap " FIRMWARE_VERSION);

  // Our config lives in LittleFS; don't also write credentials to the SDK's flash area
  WiFi.persistent(false);

  String mac = WiFi.macAddress();
  writeStatus("mac", mac);

  // Try to load configuration
  configLoaded = loadConfig();

  if (!configLoaded) {
    // No config found - start configuration portal
    writeStatus("mode", "Starting configuration portal");
    startConfigPortal();
  } else if (connectToWiFi()) {
    // Config found and connected - check for updates
    writeStatus("mode", "Normal operation mode");
    checkForUpdates();
    lastUpdateCheck = millis();
  } else {
    // Config found but WiFi unreachable - let the user fix it via the portal
    writeError("boot", "Failed to connect to WiFi - starting configuration portal");
    startConfigPortal();
  }
}

void loop() {
  if (portalMode) {
    dnsServer.processNextRequest();
    server.handleClient();

    // Fallback portal (we have a saved config): if nobody is using it,
    // reboot and retry WiFi in case the network was only temporarily down.
    if (configLoaded && millis() - lastPortalActivity > PORTAL_IDLE_TIMEOUT) {
      writeStatus("portal", "Idle timeout - rebooting to retry WiFi");
      delay(100);
      ESP.restart();
    }
    return;
  }

  // Normal operation mode

  // Check WiFi connection
  if (WiFi.status() != WL_CONNECTED) {
    writeStatus("wifi", "Disconnected - attempting to reconnect");
    if (connectToWiFi()) {
      writeStatus("wifi", "Reconnected successfully");
      reconnectFailures = 0;
    } else if (++reconnectFailures >= MAX_RECONNECT_FAILS) {
      // Reboot; if WiFi is still unreachable at boot, the portal starts
      writeError("wifi", "Too many reconnect failures - rebooting");
      delay(100);
      ESP.restart();
    }
  }

  // Periodically check for OTA updates
  if (millis() - lastUpdateCheck > UPDATE_CHECK_INTERVAL) {
    if (WiFi.status() == WL_CONNECTED) {
      checkForUpdates();
    }
    lastUpdateCheck = millis();
  }

  // Small delay to prevent tight loop
  delay(1000);
}
