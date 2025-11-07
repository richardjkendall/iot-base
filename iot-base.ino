/*
 * ESP8266 IoT Bootstrap Template
 *
 * This is a minimal bootstrap firmware for ESP8266 devices (ESP-01 compatible).
 *
 * On first boot:
 * 1. Creates WiFi access point "ESP-SETUP-[MAC]" (password: "configure")
 * 2. Serves configuration portal at 192.168.4.1
 * 3. User configures WiFi credentials and API server
 * 4. Saves config and reboots
 *
 * On subsequent boots:
 * 1. Connects to configured WiFi
 * 2. Contacts API server with device MAC address
 * 3. Checks for OTA firmware updates
 * 4. If update available, downloads and installs it
 * 5. New firmware takes over
 *
 * Factory Reset: Reflash this firmware to reconfigure device
 */

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// Configuration
#define DEBUG              1
#define MAX_WIFI_WAIT      20
#define CONFIG_FILE        "/config.json"
#define AP_SSID_PREFIX     "ESP-SETUP-"
#define AP_PASSWORD        "configure"
#define DNS_PORT           53
#define UPDATE_CHECK_INTERVAL 60000  // Check for updates every 60 seconds

// Global variables
String wifiSsid = "";
String wifiPassword = "";
String apiServer = "";
int apiPort = 80;
bool configLoaded = false;
unsigned long lastUpdateCheck = 0;

// Global clients
WiFiClient wifiClient;
HTTPClient http;
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

  wifiSsid = doc["wifi_ssid"].as<String>();
  wifiPassword = doc["wifi_password"].as<String>();
  apiServer = doc["api_server"].as<String>();
  apiPort = doc["api_port"] | 80;

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
  WiFi.begin(wifiSsid, wifiPassword);

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

  t_httpUpdate_return ret = ESPhttpUpdate.update(wifiClient, url, "");

  switch(ret) {
    case HTTP_UPDATE_FAILED:
      writeError("update", "Update failed: " + ESPhttpUpdate.getLastErrorString());
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
  "</style></head><body>";

const char* HTML_FOOTER = "</body></html>";

void handleRoot() {
  String macAddress = WiFi.macAddress();

  String html = HTML_HEAD;
  html += "<div class='container'>";
  html += "<h1>ESP8266 Setup</h1>";
  html += "<div class='info'>Configure your device to connect to WiFi and your API server.<br><strong>Device MAC:</strong> " + macAddress + "</div>";
  html += "<form action='/save' method='POST'>";

  // WiFi SSID dropdown with scan results
  html += "<label>WiFi Network:</label>";
  html += "<select name='ssid' id='ssid'>";

  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    html += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }
  html += "<option value='custom'>Custom SSID...</option>";
  html += "</select>";

  html += "<input type='text' name='custom_ssid' id='custom_ssid' placeholder='Enter custom SSID' style='display:none;margin-top:10px'>";

  html += "<label>WiFi Password:</label>";
  html += "<input type='password' name='password' id='password' required>";
  html += "<label style='font-size:14px;margin-top:8px'><input type='checkbox' id='showPassword' style='width:auto;margin-right:5px'> Show password</label>";

  html += "<label>API Server (IP or hostname):</label>";
  html += "<input type='text' name='api_server' placeholder='e.g., 192.168.1.100 or api.example.com' required>";

  html += "<label>API Port:</label>";
  html += "<input type='number' name='api_port' value='80' required>";

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

void handleSave() {
  String selectedSsid = server.arg("ssid");

  if (selectedSsid == "custom") {
    wifiSsid = server.arg("custom_ssid");
  } else {
    wifiSsid = selectedSsid;
  }

  wifiPassword = server.arg("password");
  apiServer = server.arg("api_server");
  apiPort = server.arg("api_port").toInt();

  if (saveConfig()) {
    String html = HTML_HEAD;
    html += "<div class='container'>";
    html += "<h1>Configuration Saved!</h1>";
    html += "<div class='info'>Device will reboot and connect to your WiFi network.</div>";
    html += "<p>SSID: " + wifiSsid + "</p>";
    html += "<p>API Server: " + apiServer + ":" + String(apiPort) + "</p>";
    html += "</div>";
    html += HTML_FOOTER;

    server.send(200, "text/html", html);

    delay(2000);
    ESP.restart();
  } else {
    String html = HTML_HEAD;
    html += "<div class='container'>";
    html += "<h1>Error</h1>";
    html += "<p>Failed to save configuration. Please try again.</p>";
    html += "<a href='/'>Go Back</a>";
    html += "</div>";
    html += HTML_FOOTER;

    server.send(500, "text/html", html);
  }
}

void handleNotFound() {
  // Redirect all unknown requests to root (captive portal behavior)
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void startConfigPortal() {
  String macAddress = WiFi.macAddress();
  macAddress.replace(":", "");
  String apName = AP_SSID_PREFIX + macAddress.substring(6);

  WiFi.mode(WIFI_AP);
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
  writeStatus("boot", "ESP8266 Bootstrap Template v1.0");

  String mac = WiFi.macAddress();
  writeStatus("mac", mac);

  // Try to load configuration
  configLoaded = loadConfig();

  if (!configLoaded) {
    // No config found - start configuration portal
    writeStatus("mode", "Starting configuration portal");
    startConfigPortal();
  } else {
    // Config found - try to connect and get updates
    writeStatus("mode", "Normal operation mode");

    if (connectToWiFi()) {
      // Connected successfully - check for updates
      checkForUpdates();
      lastUpdateCheck = millis();
    } else {
      // Failed to connect - might want to fall back to config portal
      writeError("boot", "Failed to connect to WiFi - continuing anyway");
    }
  }
}

void loop() {
  if (!configLoaded) {
    // We're in config portal mode
    dnsServer.processNextRequest();
    server.handleClient();
  } else {
    // We're in normal operation mode

    // Check WiFi connection
    if (WiFi.status() != WL_CONNECTED) {
      writeStatus("wifi", "Disconnected - attempting to reconnect");
      if (connectToWiFi()) {
        writeStatus("wifi", "Reconnected successfully");
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
}
