/*
  ------------------------------------------------------------
  LED Matrix Companion v4 Satellite (ESP8266)
  Author: Adrian Davis
  
  Hardware:
    - ESP8266 (e.g. Wemos D1 mini / NodeMCU / custom ESP8266MOD)
    - 32x8 LED matrix (4× MAX7219 modules in a chain)

  Features:
    - Companion v4 Satellite API support over TCP (WiFiClient)
    - WiFiManager config portal (SSID = LED_Matrix_<MAC>)
    - Stores Companion IP and port in EEPROM
    - DeviceID = "LED_Matrix_" + full MAC (no colons, uppercase)
    - Text only:
        * Uses Satellite KEY-STATE TEXT="..." (base64 decoded)
        * Centers text if it fits on 32 columns
        * Scrolls right-to-left if too long
        * Restarts animation only for scrolling text
    - BRIGHTNESS 0–100 mapped to matrix intensity 0–15

  Config behaviour:
    - WiFi fails => WiFiManager config portal (standard)
    - Companion down => keep retrying, NO config portal
    - Hold DOWNLOAD for 2 seconds after boot => setup menu

  Background handling modes (configurable via WiFiManager):
    - none    : ignore COLOR, no invert, no bars
    - invert  : invert the whole display based on COLOR
    - bars    : left+right 2 columns ON if any channel >=128
    - pgmpvw  : left 2 = red>=128 (PGM), right 2 = green>=128 (PVW)
    - pvwpgm  : left 2 = green>=128 (PVW), right 2 = red>=128 (PGM)
  
  Library Modification:
    - ESP8266mDNS library modified to increase MDNS_SERVICE_NAME_LENGTH
    - Changed from 15 to 25 characters in LEAmDNS.h line 161
    - Required to support "companion-satellite" service name (19 chars)
    - Without this modification, service registration fails due to length validation
  ------------------------------------------------------------
*/

#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <EEPROM.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <vector>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <Updater.h>

// The stock ESP8266 core limits mDNS service labels to 15 characters, while
// Companion discovers `_companion-satellite._tcp` (19 characters).
#if defined(MDNS_SERVICE_NAME_LENGTH) && MDNS_SERVICE_NAME_LENGTH < 19
#warning "Patch ESP8266mDNS MDNS_SERVICE_NAME_LENGTH to at least 19; see patches/esp8266-mdns-service-name-length.patch"
#endif

// ------------------------ MATRIX CONFIG ---------------------

#define HARDWARE_TYPE MD_MAX72XX::PAROLA_HW
#define MAX_DEVICES   4

// Pins for ESP8266 (numeric GPIOs)
#define PIN_CS   15    // D8 on many dev boards
#define PIN_CLK  14    // D5
#define PIN_DIN  13    // D7
#define PIN_DOWNLOAD 0 // GPIO0 / D3: DOWNLOAD button, active-low after boot

MD_Parola P = MD_Parola(HARDWARE_TYPE, PIN_CS, MAX_DEVICES);
// Underlying MAX72xx object for direct column control (bars)
MD_MAX72XX* mx = nullptr;

// Global text buffer that Parola will use (must stay valid!)
char matrixText[96];   // adjust size if you want longer max text

// Track whether current text is scrolling or static
bool textScrolls = false;

// Parola timing
const uint16_t scrollSpeed = 40;     // Lower = slower
const uint16_t scrollPause = 0;      // Pause at end of scroll

// Boot prompt timing
const unsigned long CONFIG_PROMPT_MS = 2900;

// ------------------------ BACKGROUND / COLOR MODE -----------

enum BackgroundMode {
  BG_NONE = 0,
  BG_INVERT,
  BG_BARS,
  BG_PGMPVW,
  BG_PVWPGM
};

BackgroundMode bgMode = BG_INVERT;          // default behaviour

// Stored as text for WiFiManager & EEPROM
char bg_mode_str[16] = "invert";

// Last COLOR received
int  lastColorR = 0;
int  lastColorG = 0;
int  lastColorB = 0;
bool lastColorValid = false;

// Current bar states
bool barLeftOn  = false;
bool barRightOn = false;

// Invert flag based on background colour (for BG_INVERT)
bool invertDisplay = false;

// ------------------------ COMPANION CONFIG ------------------

WiFiManager wifiManager;
WiFiClient  client;

// REST API Server for Companion configuration
ESP8266WebServer restServer(9999);

// What we store in EEPROM
char companion_host[40] = "Companion IP";
char companion_port[6]  = "16622";

// WiFiManager custom params
WiFiManagerParameter* custom_companionIP;
WiFiManagerParameter* custom_companionPort;
WiFiManagerParameter* custom_bgMode;     // background handling mode

// Device ID and hostname
String deviceID;
bool mdnsStarted = false;

// GPIO0 is a boot strap pin: holding DOWNLOAD while resetting starts the
// serial bootloader. Once the sketch is running, a deliberate long press is
// safe to use as the configuration trigger.
const unsigned long downloadButtonHoldMs = 2000;
unsigned long downloadButtonPressedAt = 0;
bool downloadButtonHandled = false;

// AP password for config portal (blank = open)
const char* AP_password = "";

// EEPROM layout
// [0] = 'L', [1] = 'M' magic
// [2] = version
// [3..42]  = companion_host (40 bytes)
// [43..48] = companion_port (6 bytes)
// [50..65] = bg_mode_str (16 bytes)
// [66]     = brightness (0-100)
const uint16_t EEPROM_SIZE      = 128;
const uint16_t EEPROM_STARTUP_ACTION_ADDR = 67;

enum StartupAction : uint8_t {
  STARTUP_NORMAL = 0,
  STARTUP_WEB_CONFIG = 1,
  STARTUP_WIFI_AP = 2,
};

// Timing / connection
unsigned long lastPingTime     = 0;
unsigned long lastConnectTry   = 0;
const unsigned long connectRetryMs  = 15000;
const unsigned long pingIntervalMs  = 2000;

// Brightness write debouncing
unsigned long lastBrightnessChange = 0;
bool brightnessWritePending = false;
const unsigned long brightnessWriteDelay = 5000; // 5 seconds

// WiFi OK / MAC alternation
unsigned long lastWiFiOKToggle = 0;
bool showMACAddress = false;
const unsigned long wifiOKToggleDelay = 5000; // 5 seconds
String macShort = ""; // First 5 chars of MAC address
bool firstConnectionAttemptMade = false; // Track if first connection attempt was made

// Delayed Connecting... message
unsigned long connectionStartTime = 0;
bool connectingMessageShown = false;
const unsigned long connectingMessageDelay = 1000; // 1 second
bool connectionEstablished = false; // Track when connection is fully established

// Brightness (0–100 from Companion)
int brightness = 1;

// ------------------------ TEXT STATE ------------------------

String currentText  = "";
String pendingText  = "";
bool   textDirty    = false;

// If you later add real big/small fonts, you can switch based on content
bool   useBigFont   = true;

// ------------------------------------------------------------
// Simple Base64 decoder (local, no extra libs)
// ------------------------------------------------------------
const char* B64_TABLE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64Index(char c) {
  const char* p = strchr(B64_TABLE, c);
  if (!p) return -1;
  return (int)(p - B64_TABLE);
}

String decodeBase64(const String& input) {
  int len = input.length();
  int val = 0;
  int valb = -8;
  String out;

  for (int i = 0; i < len; i++) {
    char c = input[i];
    if (c == '=') break;
    int idx = b64Index(c);
    if (idx < 0) break;  // invalid char – stop decoding

    val = (val << 6) + idx;
    valb += 6;
    if (valb >= 0) {
      char outChar = (char)((val >> valb) & 0xFF);
      out += outChar;
      valb -= 8;
    }
  }

  return out;
}

// ------------------------------------------------------------
// EEPROM Helpers
// ------------------------------------------------------------
extern String firmwareUpdatePassword;
void eepromLoadCompanionConfig() {
  EEPROM.begin(EEPROM_SIZE);
  char updatePassword[33] = {};
  for (uint8_t i = 0; i < 32; ++i) updatePassword[i] = EEPROM.read(70 + i);
  firmwareUpdatePassword = String(updatePassword);
  if (EEPROM.read(0) == 'L' && EEPROM.read(1) == 'M') {
    // Valid header
    for (int i = 0; i < 40; i++) {
      companion_host[i] = (char)EEPROM.read(3 + i);
    }
    companion_host[39] = '\0';

    for (int i = 0; i < 6; i++) {
      companion_port[i] = (char)EEPROM.read(43 + i);
    }
    companion_port[5] = '\0';

    // bg_mode_str
    for (int i = 0; i < 16; i++) {
      bg_mode_str[i] = (char)EEPROM.read(50 + i);
    }
    bg_mode_str[15] = '\0';

    // brightness
    brightness = EEPROM.read(66);
    // Validate brightness range
    if (brightness < 0 || brightness > 100) {
      brightness = 1; // default if invalid
    }
  } else {
    // No valid data - set defaults
    strcpy(companion_host, "192.168.1.100");
    strcpy(companion_port, "9999");
    strcpy(bg_mode_str, "invert");
    brightness = 1; // default brightness
  }
  EEPROM.end();
}

void eepromSaveCompanionConfig(const char* host, const char* port) {
  EEPROM.begin(EEPROM_SIZE);

  EEPROM.write(0, 'L');
  EEPROM.write(1, 'M');
  EEPROM.write(2, 2); // version 2 (with bg_mode_str)

  // host
  for (int i = 0; i < 40; i++) {
    char c = (i < (int)strlen(host)) ? host[i] : 0;
    EEPROM.write(3 + i, (uint8_t)c);
  }

  // port
  for (int i = 0; i < 6; i++) {
    char c = (i < (int)strlen(port)) ? port[i] : 0;
    EEPROM.write(43 + i, (uint8_t)c);
  }

  // bg_mode_str (skip address 49 for alignment)
  for (int i = 0; i < 16; i++) {
    char c = (i < (int)strlen(bg_mode_str)) ? bg_mode_str[i] : 0;
    EEPROM.write(50 + i, (uint8_t)c);
  }

  // brightness
  EEPROM.write(66, (uint8_t)brightness);

  EEPROM.commit();
  EEPROM.end();
}

StartupAction eepromReadStartupAction() {
  EEPROM.begin(EEPROM_SIZE);
  uint8_t value = EEPROM.read(EEPROM_STARTUP_ACTION_ADDR);
  EEPROM.end();

  return value == STARTUP_WEB_CONFIG ? STARTUP_WEB_CONFIG
       : value == STARTUP_WIFI_AP ? STARTUP_WIFI_AP
       : STARTUP_NORMAL;
}

void eepromWriteStartupAction(StartupAction action) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(EEPROM_STARTUP_ACTION_ADDR, action);
  EEPROM.commit();
  EEPROM.end();
}

// ------------------------------------------------------------
// Background mode helpers (parsing & bar drawing)
// ------------------------------------------------------------

// Helper: uppercase copy of a String
String toUpperCaseStr(const String &s) {
  String r = s;
  for (size_t i = 0; i < r.length(); i++) {
    r[i] = toupper(r[i]);
  }
  return r;
}

// Parse textual bg_mode_str into enum
BackgroundMode parseBgMode(const char* val) {
  if (!val || !val[0]) return BG_INVERT;  // default

  String s = String(val);
  s.toLowerCase();

  if (s == "none")   return BG_NONE;
  if (s == "invert") return BG_INVERT;
  if (s == "bars")   return BG_BARS;
  if (s == "pgmpvw") return BG_PGMPVW;
  if (s == "pvwpgm") return BG_PVWPGM;

  return BG_INVERT;
}

// Draw/update edge bars based on booleans
void updateBackgroundBars(bool leftOn, bool rightOn) {
  barLeftOn  = leftOn;
  barRightOn = rightOn;

  if (!mx) return;

  uint8_t colOn  = 0xFF;  // all 8 rows lit in that column
  uint8_t colOff = 0x00;

  uint8_t maxCols = MAX_DEVICES * 8;  // 32 for 4 modules

  // Leftmost 2 columns: 0, 1
  mx->setColumn(0, leftOn ? colOn : colOff);
  mx->setColumn(1, leftOn ? colOn : colOff);

  // Rightmost 2 columns: maxCols-2, maxCols-1
  uint8_t c0 = maxCols - 2;
  uint8_t c1 = maxCols - 1;
  mx->setColumn(c0, rightOn ? colOn : colOff);
  mx->setColumn(c1, rightOn ? colOn : colOff);
}

// Apply background/invert/bars for a given color and current bgMode
void applyBackgroundFromColor(int r, int g, int b) {
  bool anyBright   = (r >= 128) || (g >= 128) || (b >= 128);
  bool redBright   = (r >= 128);
  bool greenBright = (g >= 128);

  bool left = false;
  bool right = false;

  switch (bgMode) {
    case BG_NONE:
      invertDisplay = false;
      P.setInvert(false);
      left = right = false;
      break;

    case BG_INVERT:
      invertDisplay = anyBright;
      P.setInvert(invertDisplay);
      left = right = false;  // no bars in this mode
      break;

    case BG_BARS:
      // Bars like invert's threshold: anyBright toggles both sides
      invertDisplay = false;
      P.setInvert(false);
      if (anyBright) {
        left  = true;
        right = true;
      }
      break;

    case BG_PGMPVW:
      // PGM on left, PVW on right
      invertDisplay = false;
      P.setInvert(false);
      if (redBright)   left  = true;  // PGM
      if (greenBright) right = true;  // PVW
      break;

    case BG_PVWPGM:
      // PVW on left, PGM on right
      invertDisplay = false;
      P.setInvert(false);
      if (greenBright) left  = true;  // PVW
      if (redBright)   right = true;  // PGM
      break;
  }

  updateBackgroundBars(left, right);

  Serial.printf(
    "[BG] Mode=%d  R=%d G=%d B=%d  invert=%s  left=%s right=%s\n",
    (int)bgMode, r, g, b,
    invertDisplay ? "true" : "false",
    left ? "ON" : "OFF",
    right ? "ON" : "OFF"
  );
}

// Re-apply background mode to the last known COLOR (e.g. after config change)
void applyBackgroundFromLastColor() {
  if (lastColorValid) {
    applyBackgroundFromColor(lastColorR, lastColorG, lastColorB);
  } else {
    // No color yet: treat as black
    applyBackgroundFromColor(0, 0, 0);
  }
}

// ------------------------------------------------------------
// WiFiManager helpers
// ------------------------------------------------------------

String getParam(const String& name) {
  if (wifiManager.server && wifiManager.server->hasArg(name)) {
    return wifiManager.server->arg(name);
  }
  return "";
}

void saveParamCallback() {
  String str_companionIP   = getParam("companionIP");
  String str_companionPort = getParam("companionPort");
  String str_bgMode        = getParam("bgmode");

  if (str_companionIP.length() > 0) {
    str_companionIP.toCharArray(companion_host, sizeof(companion_host));
  }
  if (str_companionPort.length() > 0) {
    str_companionPort.toCharArray(companion_port, sizeof(companion_port));
  }
  if (str_bgMode.length() > 0) {
    str_bgMode.trim();  // strip accidental spaces etc.
    str_bgMode.toCharArray(bg_mode_str, sizeof(bg_mode_str));
    bg_mode_str[sizeof(bg_mode_str) - 1] = '\0';
  }

  // Update bgMode from updated string
  bgMode = parseBgMode(bg_mode_str);
  applyBackgroundFromLastColor();

  eepromSaveCompanionConfig(companion_host, companion_port);
  Serial.println("[WiFi] Settings saved");
}

// ------------------------------------------------------------
// Matrix helpers
// ------------------------------------------------------------

void setMatrixBrightnessFromPercent(int percent) {
  if (percent < 0)   percent = 0;
  if (percent > 100) percent = 100;

  // Map 0–100 → 0–15
  uint8_t intensity = map(percent, 0, 100, 0, 15);
  P.setIntensity(intensity);
}

void setTextNow(const String& txt);

// Simple helper to show a boot/config message
void showBootMessage(const String& msg) {
  setTextNow(msg);
}

// Decide whether we can center text or need to scroll
// Very approximate: assume ~6px per char on default font.
void applyTextToParola() {
  String txt = currentText;

  if (txt.length() == 0) {
    P.displayClear();
    textScrolls = false;
    return;
  }

  // lowercase detection left in, if you want to change font later
  bool hasLower = false;
  for (uint16_t i = 0; i < txt.length(); i++) {
    if (isLowerCase(txt[i])) {
      hasLower = true;
      break;
    }
  }
  useBigFont = !hasLower;

  // Use default internal font
  P.setFont(nullptr);

  // Apply invert state (for BG_INVERT only)
  P.setInvert(invertDisplay);

  // Copy into global matrixText buffer and NUL terminate
  size_t len = txt.length();
  if (len >= sizeof(matrixText)) {
    len = sizeof(matrixText) - 1; // truncate if too long
  }
  for (size_t i = 0; i < len; i++) {
    matrixText[i] = txt[i];
  }
  matrixText[len] = '\0';

  uint16_t maxCols    = MAX_DEVICES * 8;
  uint16_t charWidth  = useBigFont ? 8 : 6;
  uint16_t textWidth  = len * charWidth;
  bool textFits       = (textWidth <= maxCols);

  // Also treat “short” text as fitting even if estimate is wrong
  textFits = textFits || (len <= 5);
  
  // Specific override: WiFi ! should always be static
  if (txt == "WiFi !") {
    textFits = true;
  }
  
  // Specific override: WiFi ? should always be static
  if (txt == "WiFi ?") {
    textFits = true;
  }
  
  // Specific override: Hello! should always be static
  if (txt == "Hello!") {
    textFits = true;
  }

  P.displayClear();

  if (textFits) {
    // Static, centred text – no animation, no flashing
    textScrolls = false;
    P.setTextAlignment(PA_CENTER);
    P.print(matrixText);
  } else {
    // Scrolling text (right-to-left on your flipped modules)
    textScrolls = true;
    P.displayText(
      matrixText,
      PA_LEFT,
      scrollSpeed,
      scrollPause,
      PA_SCROLL_RIGHT,   // entry
      PA_SCROLL_RIGHT    // exit
    );
    P.displayReset();
  }

  // Reapply bars on top of whatever we just drew (modes that use bars) - only when Companion is connected
  if (client.connected() && (bgMode == BG_BARS || bgMode == BG_PGMPVW || bgMode == BG_PVWPGM)) {
    updateBackgroundBars(barLeftOn, barRightOn);
  }
}

void setTextNow(const String& txt) {
  currentText = txt;
  applyTextToParola();
}

void setText(const String& txt) {
  pendingText = txt;
  textDirty   = true;
}

// ------------------------------------------------------------
// Colour parsing helpers for KEY-STATE
// ------------------------------------------------------------

// Parse token forms like:
//   COLOR="#RRGGBB"
//   COLOR=#RRGGBB
//   COLOR="R,G,B"
//   COLOR=R,G,B
bool parseColorToken(const String& line, const String& key, int &r, int &g, int &b) {
  String upLine = toUpperCaseStr(line);
  String upKey  = toUpperCaseStr(key);

  int pos = upLine.indexOf(upKey);
  if (pos < 0) return false;

  pos += upKey.length();
  if (pos < (int)upLine.length() && upLine[pos] == '=') pos++;

  int end = upLine.indexOf(' ', pos);
  if (end < 0) end = upLine.length();

  String val = upLine.substring(pos, end);
  val.trim();
  if (val.length() == 0) return false;

  // Strip surrounding quotes if present, e.g. "#FFFFFF" -> #FFFFFF
  if (val.length() >= 2 && val[0] == '"' && val[val.length() - 1] == '"') {
    val = val.substring(1, val.length() - 1);
    val.trim();
    if (val.length() == 0) return false;
  }

  // Hex form: #RRGGBB
  if (val[0] == '#') {
    if (val.length() < 7) return false;
    String rs = val.substring(1, 3);
    String gs = val.substring(3, 5);
    String bs = val.substring(5, 7);
    r = (int) strtol(rs.c_str(), nullptr, 16);
    g = (int) strtol(gs.c_str(), nullptr, 16);
    b = (int) strtol(bs.c_str(), nullptr, 16);
    return true;
  }

  // Decimal CSV form: R,G,B
  int c1 = val.indexOf(',');
  int c2 = val.indexOf(',', c1 + 1);
  if (c1 < 0 || c2 < 0) return false;

  r = val.substring(0, c1).toInt();
  g = val.substring(c1 + 1, c2).toInt();
  b = val.substring(c2 + 1).toInt();
  return true;
}

void handleKeyStateColor(const String& line) {
  int r, g, b;
  if (parseColorToken(line, "COLOR", r, g, b)) {
    lastColorR = r;
    lastColorG = g;
    lastColorB = b;
    lastColorValid = true;

    applyBackgroundFromColor(r, g, b);
  }
}

// ------------------------------------------------------------
// Companion / Satellite API parsing
// ------------------------------------------------------------

void sendAddDevice() {
  String companionDeviceID = "led-matrix:" + macShort;
  String cmd = "ADD-DEVICE DEVICEID=" + companionDeviceID +
               " PRODUCT_NAME=\"LED Matrix\" KEYS_TOTAL=1 KEYS_PER_ROW=1 BITMAPS=0 COLORS=true TEXT=true";
  client.println(cmd);
  Serial.println("[API] Sent: " + cmd);
}

// Decode Companion TEXT (base64 → UTF-8 string)
String decodeCompanionText(const String& encoded) {
  if (encoded.length() == 0) return encoded;

  // Heuristic: if it contains only base64-ish chars, try decode
  bool looksBase64 = true;
  for (size_t i = 0; i < encoded.length(); i++) {
    char c = encoded[i];
    if (!((c >= 'A' && c <= 'Z') ||
          (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') ||
          c == '+' || c == '/' || c == '=')) {
      looksBase64 = false;
      break;
    }
  }

  if (!looksBase64) {
    return encoded;
  }

  String decoded = decodeBase64(encoded);
  if (decoded.length() == 0) {
    // if decode failed, fall back to original
    return encoded;
  }
  return decoded;
}

void handleKeyStateText(const String& line) {
  int tPos = line.indexOf("TEXT=");
  if (tPos < 0) {
    return; // no text in this update
  }

  // Find the first quote after TEXT=
  int firstQuote = line.indexOf('\"', tPos);
  if (firstQuote < 0) return;

  int secondQuote = line.indexOf('\"', firstQuote + 1);
  if (secondQuote < 0) return;

  String textField = line.substring(firstQuote + 1, secondQuote);

  // Companion is sending base64 for TEXT, e.g. "MTU6NDQ=" → "15:44"
  String decoded = decodeCompanionText(textField);

  // Handle escaped newlines in the decoded string
  decoded.replace("\\n", "\n");

  Serial.print("[API] TEXT encoded = \"");
  Serial.print(textField);
  Serial.print("\"  decoded = \"");
  Serial.print(decoded);
  Serial.println("\"");

  setText(decoded);
}

void parseAPI(const String& apiData) {
  if (apiData.length() == 0) return;

  if (apiData.startsWith("PONG")) {
    return;
  }

  Serial.println("[API] RX: " + apiData);

  if (apiData.startsWith("PING")) {
    String payload = apiData.substring(apiData.indexOf(' ') + 1);
    client.println("PONG " + payload);
    return;
  }

  if (apiData.startsWith("BRIGHTNESS")) {
    int valPos = apiData.indexOf("VALUE=");
    if (valPos >= 0) {
      String v = apiData.substring(valPos + 6);
      v.trim();
      brightness = v.toInt();
      Serial.println("[API] BRIGHTNESS set to " + String(brightness));
      setMatrixBrightnessFromPercent(brightness);
      
      // Mark brightness for delayed EEPROM write (debounce)
      lastBrightnessChange = millis();
      brightnessWritePending = true;
      Serial.println("[EEPROM] Brightness change marked for delayed write");
    }
    return;
  }

  if (apiData.startsWith("KEYS-CLEAR")) {
    Serial.println("[API] KEYS-CLEAR");
    setText("");
    return;
  }

  if (apiData.startsWith("KEY-STATE")) {
    Serial.println("[API] KEY-STATE raw line = " + apiData);
    handleKeyStateColor(apiData);   // check COLOR=
    handleKeyStateText(apiData);    // now handle text
    return;
  }
}

// ------------------------------------------------------------
// CONFIG PORTAL (explicit trigger)
// ------------------------------------------------------------
void showConfigModeMessage() {
  // Short, static message that does not require animation.
  // Important because WiFiManager blocks loop(), so scrolling text
  // will not be animated while the portal is active.
  P.displayClear();
  P.setFont(nullptr);
  P.setInvert(false);          // ensure normal polarity for config
  invertDisplay = false;
  P.setTextAlignment(PA_CENTER);
  P.print("CFG !");
  textScrolls = false;
  currentText = "CFG !";

  // Clear bars in config mode (just show text)
  updateBackgroundBars(false, false);
}

void startConfigPortal() {
  Serial.println("[WiFi] Entering CONFIG PORTAL mode");
  
  // Load Companion config from EEPROM (for default field values)
  eepromLoadCompanionConfig();

  // Parse bgMode from loaded bg_mode_str
  bgMode = parseBgMode(bg_mode_str);
  applyBackgroundFromLastColor();

  // ---------- Prepare WiFiManager with params ----------
  // Companion IP / Port params
  custom_companionIP   = new WiFiManagerParameter("companionIP", "Companion IP", companion_host, 40);
  custom_companionPort = new WiFiManagerParameter("companionPort", "Satellite Port", companion_port, 6);

  // Background mode param
  custom_bgMode = new WiFiManagerParameter(
    "bgmode",
    "Background Handling (none/invert/bars/pgmpvw/pvwpgm)",
    bg_mode_str,
    16
  );

  wifiManager.addParameter(custom_companionIP);
  wifiManager.addParameter(custom_companionPort);
  wifiManager.addParameter(custom_bgMode);

  wifiManager.setSaveParamsCallback(saveParamCallback);

  std::vector<const char*> menu = { "wifi", "param", "info", "sep", "restart", "exit" };
  wifiManager.setMenu(menu);
  wifiManager.setClass("invert");  // Dark mode
  wifiManager.setConfigPortalTimeout(0); // No timeout when we explicitly call config mode

  wifiManager.setAPCallback([](WiFiManager* wm) {
    Serial.println("[WiFi] Config portal started");
    showConfigModeMessage();
  });
  
  showConfigModeMessage();

  // Start AP + portal, blocks until user saves or exits
  String shortDeviceID = "led-matrix_" + macShort;  // Use underscore format for SSID and display
  wifiManager.startConfigPortal(shortDeviceID.c_str(), AP_password);
  Serial.printf("[WiFi] Config portal started - SSID: %s\n", shortDeviceID.c_str());

  // After returning, update our Companion host/port/bgMode and persist
  strncpy(companion_host, custom_companionIP->getValue(), sizeof(companion_host));
  companion_host[sizeof(companion_host) - 1] = '\0';

  strncpy(companion_port, custom_companionPort->getValue(), sizeof(companion_port));
  companion_port[sizeof(companion_port) - 1] = '\0';

  strncpy(bg_mode_str, custom_bgMode->getValue(), sizeof(bg_mode_str));
  bg_mode_str[sizeof(bg_mode_str) - 1] = '\0';
  bgMode = parseBgMode(bg_mode_str);
  applyBackgroundFromLastColor();

  eepromSaveCompanionConfig(companion_host, companion_port);

  // Show a small message so you know it applied
  showBootMessage("CFG SAVED");
  delay(1000);
}

void animateMenuDisplay() {
  if (P.displayAnimate() && textScrolls) P.displayReset();
  yield();
}

void waitForDownloadButtonRelease() {
  while (digitalRead(PIN_DOWNLOAD) == LOW) {
    animateMenuDisplay();
  }
  delay(40);  // debounce the release before accepting a menu press
}

void factoryReset() {
  Serial.println("[Setup] Factory reset requested");
  showBootMessage("RESET");

  wifiManager.resetSettings();

  EEPROM.begin(EEPROM_SIZE);
  for (uint16_t address = 0; address < EEPROM_SIZE; address++) {
    EEPROM.write(address, 0);
  }
  EEPROM.commit();
  EEPROM.end();

  delay(750);
  ESP.restart();
}

void startWebConfigPortal() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Setup] No WiFi for web config; opening WiFi AP instead");
    startConfigPortal();
    ESP.restart();
  }

  Serial.println("[Setup] Starting web configuration portal");
  wifiManager.setConfigPortalBlocking(false);
  wifiManager.startWebPortal();
  showBootMessage(String("WEB ") + WiFi.localIP().toString());
  waitForDownloadButtonRelease();

  unsigned long pressedAt = 0;
  while (true) {
    wifiManager.process();
    animateMenuDisplay();

    if (digitalRead(PIN_DOWNLOAD) == LOW) {
      if (pressedAt == 0) pressedAt = millis();
      if (millis() - pressedAt >= downloadButtonHoldMs) {
        Serial.println("[Setup] Closing web configuration portal");
        wifiManager.stopWebPortal();
        ESP.restart();
      }
    } else {
      pressedAt = 0;
    }
  }
}

void runSetupMenu() {
  static const char* const menuLabels[] = { "NORMAL", "WEB CFG", "WIFI AP", "RESET" };
  const uint8_t menuCount = sizeof(menuLabels) / sizeof(menuLabels[0]);
  uint8_t selected = 0;

  Serial.println("[Setup] Menu: short press = next, hold 2s = select");
  showBootMessage(menuLabels[selected]);
  waitForDownloadButtonRelease();

  unsigned long pressedAt = 0;
  bool wasPressed = false;

  while (true) {
    animateMenuDisplay();
    bool pressed = digitalRead(PIN_DOWNLOAD) == LOW;

    if (pressed && !wasPressed) {
      pressedAt = millis();
    } else if (!pressed && wasPressed) {
      if (millis() - pressedAt < downloadButtonHoldMs) {
        selected = (selected + 1) % menuCount;
        showBootMessage(menuLabels[selected]);
        Serial.printf("[Setup] Selected %s\n", menuLabels[selected]);
      }
      pressedAt = 0;
    }

    if (pressed && pressedAt != 0 && millis() - pressedAt >= downloadButtonHoldMs) {
      Serial.printf("[Setup] Selected %s\n", menuLabels[selected]);
      waitForDownloadButtonRelease();
      wasPressed = false;
      pressedAt = 0;

      switch (selected) {
        case 0:  // Normal boot
          return;

        case 1:  // Web config on the current WiFi network
          eepromWriteStartupAction(STARTUP_WEB_CONFIG);
          ESP.restart();
          break;

        case 2:  // WiFi AP configuration portal
          eepromWriteStartupAction(STARTUP_WIFI_AP);
          ESP.restart();
          break;

        case 3: {  // Factory reset requires a second explicit confirmation
          bool confirm = false;
          showBootMessage("NO RESET");
          Serial.println("[Setup] Factory reset: short press toggles NO/YES, hold 2s confirms");

          while (true) {
            animateMenuDisplay();
            bool confirmPressed = digitalRead(PIN_DOWNLOAD) == LOW;

            if (confirmPressed && !wasPressed) {
              pressedAt = millis();
            } else if (!confirmPressed && wasPressed) {
              if (millis() - pressedAt < downloadButtonHoldMs) {
                confirm = !confirm;
                showBootMessage(confirm ? "YES RESET" : "NO RESET");
              }
              pressedAt = 0;
            }

            if (confirmPressed && pressedAt != 0 && millis() - pressedAt >= downloadButtonHoldMs) {
              waitForDownloadButtonRelease();
              if (confirm) factoryReset();

              showBootMessage(menuLabels[selected]);
              break;
            }

            wasPressed = confirmPressed;
          }
          break;
        }
      }
    }

    wasPressed = pressed;
  }
}

void handleDownloadButton(unsigned long now) {
  if (downloadButtonHandled) return;

  if (digitalRead(PIN_DOWNLOAD) == LOW) {
    if (downloadButtonPressedAt == 0) {
      downloadButtonPressedAt = now;
      return;
    }

    if (now - downloadButtonPressedAt >= downloadButtonHoldMs) {
      downloadButtonHandled = true;
      Serial.println("[Button] DOWNLOAD held for 2 seconds; opening setup menu");

      if (client.connected()) client.stop();
      runSetupMenu();
      downloadButtonHandled = false;
      downloadButtonPressedAt = 0;
    }
  } else {
    downloadButtonPressedAt = 0;
  }
}

// ------------------------------------------------------------
// WiFi / Initial Config logic
// ------------------------------------------------------------

void connectToNetwork() {
  WiFi.mode(WIFI_STA);

  // Load Companion config from EEPROM (for default field values)
  eepromLoadCompanionConfig();

  // Parse bgMode from loaded bg_mode_str
  bgMode = parseBgMode(bg_mode_str);
  applyBackgroundFromLastColor();

  // ---------- Prepare WiFiManager with params BEFORE any portal ----------

  // Companion IP / Port params
  custom_companionIP   = new WiFiManagerParameter("companionIP", "Companion IP", companion_host, 40);
  custom_companionPort = new WiFiManagerParameter("companionPort", "Satellite Port", companion_port, 6);

  // Background mode param
  custom_bgMode = new WiFiManagerParameter(
    "bgmode",
    "Background Handling (none/invert/bars/pgmpvw/pvwpgm)",
    bg_mode_str,
    sizeof(bg_mode_str)
  );

  wifiManager.addParameter(custom_companionIP);
  wifiManager.addParameter(custom_companionPort);
  wifiManager.addParameter(custom_bgMode);

  wifiManager.setSaveParamsCallback(saveParamCallback);

  std::vector<const char*> menu = { "wifi", "param", "info", "sep", "restart", "exit" };
  wifiManager.setMenu(menu);
  wifiManager.setClass("invert");
  wifiManager.setConfigPortalTimeout(180); // 3 minutes auto portal if WiFi fails

  wifiManager.setAPCallback([](WiFiManager* wm) {
    Serial.println("[WiFi] Config portal started");
    showConfigModeMessage();
  });

  // -----------------------------------------------------------------------
  // Boot counter logic is now handled in setup() - no checks needed here
  // -----------------------------------------------------------------------

  // Normal autoConnect behaviour (connect to WiFi, or start portal if no WiFi)
  // Show "WiFi" during connection attempt
  showBootMessage("WiFi ?");
  
  // Use shortened device ID for WiFi portal name (underscore format)
  String shortDeviceID = "led-matrix_" + macShort;  // Use underscore format for SSID and display
  bool res = wifiManager.autoConnect(shortDeviceID.c_str(), AP_password);
  Serial.printf("[WiFi] AutoConnect - SSID: %s\n", shortDeviceID.c_str());

  if (!res) {
    Serial.println("[WiFi] Failed to connect, starting config portal...");
    showBootMessage("CFG !");
    // WiFiManager will automatically start config portal on failure
    // No need to restart - let WiFiManager handle it
  } else {
    Serial.print("[WiFi] Connected: ");
    Serial.println(WiFi.localIP());
    // Display WiFi OK immediately after connection succeeds
    showBootMessage("WiFi !");
  }

  // Copy latest values (including bgMode)
  strncpy(companion_host, custom_companionIP->getValue(), sizeof(companion_host));
  companion_host[sizeof(companion_host) - 1] = '\0';

  strncpy(companion_port, custom_companionPort->getValue(), sizeof(companion_port));
  companion_port[sizeof(companion_port) - 1] = '\0';

  strncpy(bg_mode_str, custom_bgMode->getValue(), sizeof(bg_mode_str));
  bg_mode_str[sizeof(bg_mode_str) - 1] = '\0';
  bgMode = parseBgMode(bg_mode_str);
  // Don't apply background yet - no color data and it clears WiFi ! message

  eepromSaveCompanionConfig(companion_host, companion_port);

}

// ------------------------------------------------------------
// REST API Handlers for Companion Configuration
// ------------------------------------------------------------
void handleGetHost() {
  restServer.send(200, "text/plain", companion_host);
  Serial.println("[REST] GET /api/host: " + String(companion_host));
}

void handleGetPort() {
  restServer.send(200, "text/plain", companion_port);
  Serial.println("[REST] GET /api/port: " + String(companion_port));
}

void handleGetConfig() {
  String json = "{\"host\":\"" + String(companion_host) + "\",\"port\":" + String(companion_port) + "}";
  restServer.send(200, "application/json", json);
  Serial.println("[REST] GET /api/config: " + json);
}

String jsonSetting(const String& body, const char* name) {
  const String key = String("\"") + name + "\"";
  int pos = body.indexOf(key); if (pos < 0) return "";
  pos = body.indexOf(':', pos + key.length()); if (pos < 0) return "";
  pos++; while (pos < body.length() && isspace(body[pos])) pos++;
  if (pos < body.length() && body[pos] == '\"') { const int end = body.indexOf('\"', ++pos); return end < 0 ? "" : body.substring(pos, end); }
  int end = pos; while (end < body.length() && body[end] != ',' && body[end] != '}') end++;
  String value = body.substring(pos, end); value.trim(); return value;
}

void handleGetSettings() {
  const String json = "{\"mode\":\"" + String(bg_mode_str) + "\",\"brightness\":" + String(brightness) + "}";
  restServer.send(200, "application/json", json);
}

void handlePostSettings() {
  const String body = restServer.arg("plain");
  const String mode = jsonSetting(body, "mode"), brightnessValue = jsonSetting(body, "brightness");
  if (mode.length() && !(mode == "none" || mode == "invert" || mode == "bars" || mode == "pgmpvw" || mode == "pvwpgm")) { restServer.send(400, "text/plain", "Invalid mode"); return; }
  if (brightnessValue.length() && (brightnessValue.toInt() < 0 || brightnessValue.toInt() > 100)) { restServer.send(400, "text/plain", "Invalid brightness"); return; }
  if (mode.length()) { mode.toCharArray(bg_mode_str, sizeof(bg_mode_str)); bgMode = parseBgMode(bg_mode_str); applyBackgroundFromLastColor(); }
  if (brightnessValue.length()) { brightness = brightnessValue.toInt(); setMatrixBrightnessFromPercent(brightness); }
  eepromSaveCompanionConfig(companion_host, companion_port);
  restServer.send(200, "application/json", "{\"ok\":true}");
}

String statusJsonEscape(String value) {
  value.replace("\\", "\\\\"); value.replace("\"", "\\\"");
  value.replace("\n", "\\n"); value.replace("\r", "\\r");
  return value;
}

void handleStatus() {
  String json = "{\"deviceName\":\"LED Matrix Clock\",\"deviceId\":\"" + statusJsonEscape(deviceID) + "\",";
  json += "\"network\":\"wifi\",\"networkConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  json += "\"ssid\":\"" + statusJsonEscape(WiFi.SSID()) + "\",\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"companionConnected\":" + String(client.connected() ? "true" : "false") + ",";
  json += "\"companion\":\"" + statusJsonEscape(String(companion_host) + ":" + companion_port) + "\",";
  json += "\"text\":\"" + statusJsonEscape(currentText) + "\",\"mode\":\"" + String(bg_mode_str) +
    "\",\"brightness\":" + String(brightness) + ",";
  json += "\"color\":{\"valid\":" + String(lastColorValid ? "true" : "false") + ",\"r\":" +
    String(lastColorR) + ",\"g\":" + String(lastColorG) + ",\"b\":" + String(lastColorB) + "},";
  json += "\"uptimeSeconds\":" + String(millis() / 1000) + "}";
  restServer.send(200, "application/json", json);
}

void handleDashboard() {
  restServer.send(200, "text/html",
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><title>LED Matrix Clock</title>"
    "<h2>LED Matrix Clock Companion Satellite</h2><h3>Live troubleshooting status</h3><div id=s>Loading...</div>"
    "<p>Incoming text: <code id=t>-</code></p><p>Incoming colour: <span id=w style='display:inline-block;width:2em;height:1em;border:1px solid'></span> <code id=c>-</code></p>"
    "<p><a href=/update>Firmware update</a></p><pre id=j></pre><script>async function u(){try{let x=await(await fetch('/api/status')).json();"
    "s.textContent=(x.networkConnected?'Network connected':'Network disconnected')+' | '+(x.companionConnected?'Companion connected':'Companion disconnected')+' | '+x.ip;"
    "t.textContent=x.text||'(none)';let q=x.color;c.textContent=`rgb(${q.r}, ${q.g}, ${q.b})`;w.style.background=`rgb(${q.r},${q.g},${q.b})`;"
    "j.textContent=JSON.stringify(x,null,2)}catch(e){s.textContent='Status unavailable'}}u();setInterval(u,2000)</script>");
}

void handlePostHost() {
  String newHost = "";
  
  // Try to parse JSON first
  if (restServer.hasArg("plain")) {
    String body = restServer.arg("plain");
    body.trim();
    
    // Check if it's JSON format
    if (body.startsWith("{") && body.endsWith("}")) {
      int hostPos = body.indexOf("\"host\":");
      if (hostPos >= 0) {
        int startQuote = body.indexOf("\"", hostPos + 7);
        int endQuote = body.indexOf("\"", startQuote + 1);
        if (startQuote >= 0 && endQuote > startQuote) {
          newHost = body.substring(startQuote + 1, endQuote);
        }
      }
    } else {
      // Plain text format
      newHost = body;
    }
  }
  
  newHost.trim();
  
  if (newHost.length() > 0 && newHost.length() < sizeof(companion_host)) {
    strncpy(companion_host, newHost.c_str(), sizeof(companion_host));
    companion_host[sizeof(companion_host) - 1] = '\0';
    
    // Save to EEPROM
    eepromSaveCompanionConfig(companion_host, companion_port);
    
    // Update WiFiManager parameter
    if (custom_companionIP) {
      custom_companionIP->setValue(companion_host, sizeof(companion_host));
    }
    
    restServer.send(200, "text/plain", "OK");
    Serial.println("[REST] POST /api/host: Updated to " + String(companion_host));
    
    // Reestablish connection
    if (client.connected()) {
      client.stop();
      Serial.println("[REST] Disconnected from Companion to reconnect with new host");
    }
    lastConnectTry = 0; // Force immediate reconnection attempt
    
  } else {
    restServer.send(400, "text/plain", "Invalid host");
    Serial.println("[REST] POST /api/host: Invalid host provided");
  }
}

void handlePostPort() {
  String newPort = "";
  
  // Try to parse JSON first
  if (restServer.hasArg("plain")) {
    String body = restServer.arg("plain");
    body.trim();
    
    // Check if it's JSON format
    if (body.startsWith("{") && body.endsWith("}")) {
      int portPos = body.indexOf("\"port\":");
      if (portPos >= 0) {
        int colon = body.indexOf(":", portPos + 7);
        if (colon >= 0) {
          int endBrace = body.indexOf("}", colon);
          if (endBrace > colon) {
            newPort = body.substring(colon + 1, endBrace);
          } else {
            // If no closing brace found, take everything after colon
            newPort = body.substring(colon + 1);
          }
          newPort.trim();
        }
      }
    } else {
      // Plain text format
      newPort = body;
    }
  }
  
  newPort.trim();
  
  if (newPort.length() > 0 && newPort.length() < sizeof(companion_port)) {
    int portNum = newPort.toInt();
    if (portNum > 0 && portNum <= 65535) {
      strncpy(companion_port, newPort.c_str(), sizeof(companion_port));
      companion_port[sizeof(companion_port) - 1] = '\0';
      
      // Save to EEPROM
      eepromSaveCompanionConfig(companion_host, companion_port);
      
      // Update WiFiManager parameter
      if (custom_companionPort) {
        custom_companionPort->setValue(companion_port, sizeof(companion_port));
      }
      
      restServer.send(200, "text/plain", "OK");
      Serial.println("[REST] POST /api/port: Updated to " + String(companion_port));
      
      // Reestablish connection
      if (client.connected()) {
        client.stop();
        Serial.println("[REST] Disconnected from Companion to reconnect with new port");
      }
      lastConnectTry = 0; // Force immediate reconnection attempt
      
    } else {
      restServer.send(400, "text/plain", "Invalid port number");
      Serial.println("[REST] POST /api/port: Invalid port number: " + newPort);
    }
  } else {
    restServer.send(400, "text/plain", "Invalid port");
    Serial.println("[REST] POST /api/port: Invalid port provided");
  }
}

void handlePostConfig() {
  String newHost = "";
  String newPort = "";
  
  if (restServer.hasArg("plain")) {
    String body = restServer.arg("plain");
    body.trim();
    
    // Parse JSON
    if (body.startsWith("{") && body.endsWith("}")) {
      // Parse host
      int hostPos = body.indexOf("\"host\":");
      if (hostPos >= 0) {
        int startQuote = body.indexOf("\"", hostPos + 7);
        int endQuote = body.indexOf("\"", startQuote + 1);
        if (startQuote >= 0 && endQuote > startQuote) {
          newHost = body.substring(startQuote + 1, endQuote);
          newHost.trim();
        }
      }
      
      // Parse port
      int portPos = body.indexOf("\"port\":");
      if (portPos >= 0) {
        int colon = body.indexOf(":", portPos + 6);
        if (colon >= 0) {
          // Skip the colon and any whitespace
          int start = colon + 1;
          while (start < body.length() && (body[start] == ' ' || body[start] == '\t')) {
            start++;
          }
          
          // Look for either } or end of string
          int endBrace = body.indexOf("}", start);
          if (endBrace > start) {
            newPort = body.substring(start, endBrace);
          } else {
            // If no closing brace found, take everything after colon
            newPort = body.substring(start);
          }
          newPort.trim();
        }
      }
    }
  }
  
  // Debug output
  Serial.println("[REST] DEBUG: Parsed newHost='" + newHost + "' newPort='" + newPort + "'");
  
  bool hostValid = (newHost.length() > 0 && newHost.length() < sizeof(companion_host));
  bool portValid = false;
  int portNum = newPort.toInt();
  portValid = (newPort.length() > 0 && portNum > 0 && portNum <= 65535);
  
  Serial.println("[REST] DEBUG: hostValid=" + String(hostValid) + " portValid=" + String(portValid) + " portNum=" + String(portNum));
  
  if (hostValid && portValid) {
    strncpy(companion_host, newHost.c_str(), sizeof(companion_host));
    companion_host[sizeof(companion_host) - 1] = '\0';
    
    strncpy(companion_port, newPort.c_str(), sizeof(companion_port));
    companion_port[sizeof(companion_port) - 1] = '\0';
    
    // Save to EEPROM
    eepromSaveCompanionConfig(companion_host, companion_port);
    
    // Update WiFiManager parameters
    if (custom_companionIP) {
      custom_companionIP->setValue(companion_host, sizeof(companion_host));
    }
    if (custom_companionPort) {
      custom_companionPort->setValue(companion_port, sizeof(companion_port));
    }
    
    restServer.send(200, "text/plain", "OK");
    Serial.println("[REST] POST /api/config: Updated host=" + String(companion_host) + " port=" + String(companion_port));
    
    // Reestablish connection
    if (client.connected()) {
      client.stop();
      Serial.println("[REST] Disconnected from Companion to reconnect with new config");
    }
    lastConnectTry = 0; // Force immediate reconnection attempt
    
  } else {
    restServer.send(400, "text/plain", "Invalid config");
    Serial.println("[REST] POST /api/config: Invalid config provided - hostValid=" + String(hostValid) + " portValid=" + String(portValid));
  }
}

const char* firmwareUpdateUser = "admin";
// Empty by default: updates are open until the owner elects to protect them.
String firmwareUpdatePassword = "";

bool requireFirmwareUpdateAuth() {
  if (firmwareUpdatePassword.length() == 0 || restServer.authenticate(firmwareUpdateUser, firmwareUpdatePassword.c_str())) return true;
  restServer.requestAuthentication();
  return false;
}

void handleFirmwareUpdatePage() {
  if (!requireFirmwareUpdateAuth()) return;
  restServer.send(200, "text/html", "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><h2>Firmware update</h2><p>Select the ESP8266MOD release application <code>.bin</code> file. Do not power off while updating.</p><form method=POST action=/update enctype=multipart/form-data><input type=file name=firmware accept='.bin' required><button type=submit>Install and reboot</button></form><hr><h3>Optional protection</h3><form method=POST action=/update/password><input type=password name=password placeholder='Leave blank to remove'><button type=submit>Save update password</button></form>");
}

void handleFirmwareUpload() {
  if (firmwareUpdatePassword.length() && !restServer.authenticate(firmwareUpdateUser, firmwareUpdatePassword.c_str())) return;
  HTTPUpload& upload = restServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    const size_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    Update.begin(maxSketchSpace);
  }
  else if (upload.status == UPLOAD_FILE_WRITE) Update.write(upload.buf, upload.currentSize);
  else if (upload.status == UPLOAD_FILE_END) Update.end(true);
}

void handleFirmwareUpdateResult() {
  if (!requireFirmwareUpdateAuth()) return;
  const bool success = !Update.hasError();
  restServer.send(success ? 200 : 500, "text/plain", success ? "Update complete. Rebooting..." : "Firmware update failed.");
  if (success) { delay(500); ESP.restart(); }
}

void handleFirmwareUpdatePassword() {
  if (!requireFirmwareUpdateAuth()) return;
  firmwareUpdatePassword = restServer.arg("password");
  firmwareUpdatePassword = firmwareUpdatePassword.substring(0, 32);
  EEPROM.begin(EEPROM_SIZE);
  for (uint8_t i = 0; i < 33; ++i) EEPROM.write(70 + i, i < firmwareUpdatePassword.length() ? firmwareUpdatePassword[i] : 0);
  EEPROM.commit(); EEPROM.end();
  restServer.send(200, "text/plain", firmwareUpdatePassword.length() ? "Update password saved." : "Update password removed.");
}

void setupRestAPI() {
  // Register endpoints
  restServer.on("/", HTTP_GET, handleDashboard);
  restServer.on("/api/host", HTTP_GET, handleGetHost);
  restServer.on("/api/port", HTTP_GET, handleGetPort);
  restServer.on("/api/config", HTTP_GET, handleGetConfig);
  restServer.on("/api/settings", HTTP_GET, handleGetSettings);
  restServer.on("/api/status", HTTP_GET, handleStatus);
  
  restServer.on("/api/host", HTTP_POST, handlePostHost);
  restServer.on("/api/port", HTTP_POST, handlePostPort);
  restServer.on("/api/config", HTTP_POST, handlePostConfig);
  restServer.on("/api/settings", HTTP_POST, handlePostSettings);
  restServer.on("/update", HTTP_GET, handleFirmwareUpdatePage);
  restServer.on("/update", HTTP_POST, handleFirmwareUpdateResult, handleFirmwareUpload);
  restServer.on("/update/password", HTTP_POST, handleFirmwareUpdatePassword);
  
  // Start server
  restServer.begin();
  Serial.println("[REST] REST API server started on port 9999");
  Serial.println("[REST] Endpoints:");
  Serial.println("[REST]   GET  /api/host - Get current companion host");
  Serial.println("[REST]   GET  /api/port - Get current companion port");
  Serial.println("[REST]   GET  /api/config - Get companion config as JSON");
  Serial.println("[REST]   POST /api/host - Set companion host");
  Serial.println("[REST]   POST /api/port - Set companion port");
  Serial.println("[REST]   POST /api/config - Set companion host and port");
}

// ------------------------------------------------------------
// Companion mDNS discovery
// ------------------------------------------------------------

void setupMDNS() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[mDNS] WiFi is not connected; discovery unavailable");
    return;
  }

  String mDNSHostname = "led-matrix_" + macShort;
  Serial.printf("[mDNS] Starting responder as %s.local\n", mDNSHostname.c_str());

  if (!MDNS.begin(mDNSHostname.c_str())) {
    Serial.println("[mDNS] ERROR: responder failed to start");
    return;
  }

  String mDNSInstanceName = "led-matrix:" + macShort;
  MDNS.setInstanceName(mDNSInstanceName);

  if (!MDNS.addService("companion-satellite", "tcp", 9999)) {
    Serial.println("[mDNS] ERROR: could not register companion-satellite service");
    Serial.println("[mDNS] The stock ESP8266mDNS library has a 15-character service-name limit.");
    Serial.println("[mDNS] Apply patches/esp8266-mdns-service-name-length.patch and rebuild the ESP8266 core.");
    return;
  }

  // `restEnabled` is the field Companion uses to enable one-click setup via
  // POST /api/config. The remaining fields align this advertisement with the
  // AtomS3 satellite and make the service self-describing to mDNS browsers.
  MDNS.addServiceTxt("companion-satellite", "tcp", "restEnabled", "true");
  MDNS.addServiceTxt("companion-satellite", "tcp", "deviceId", macShort);
  MDNS.addServiceTxt("companion-satellite", "tcp", "prefix", "led-matrix");
  MDNS.addServiceTxt("companion-satellite", "tcp", "productName", "LED Matrix");
  MDNS.addServiceTxt("companion-satellite", "tcp", "apiVersion", "4");

  mdnsStarted = true;
  Serial.printf("[mDNS] Advertising %s on port 9999\n", mDNSInstanceName.c_str());
}

// ------------------------------------------------------------
// SETUP
// ------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("[LEDMatrix] Booting...");

  // Build deviceID from MAC: LED_Matrix_<MAC>
  WiFi.mode(WIFI_STA);
  delay(100);

  // Do not hold DOWNLOAD during reset: GPIO0 must be high for normal boot.
  // After boot, its button can safely be read as an active-low config trigger.
  pinMode(PIN_DOWNLOAD, INPUT_PULLUP);

  uint8_t mac[6];
  WiFi.macAddress(mac);

  char macBuf[13];
  sprintf(macBuf, "%02X%02X%02X%02X%02X%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  deviceID  = "LED_Matrix_";
  deviceID += macBuf;
  deviceID.toUpperCase();

  // Create short MAC string (last 5 characters) directly from MAC buffer
  macShort = String(macBuf).substring(7); // Take last 5 chars directly from MAC
  Serial.println("[MAC] Short MAC for display: " + macShort);

  Serial.println("[ID] deviceID = " + deviceID);
  // Set WiFi hostname using underscore format
  String wifiHostname = "led-matrix_" + macShort;
  WiFi.hostname(wifiHostname);
  Serial.printf("[WiFi] Hostname set to: %s\n", wifiHostname.c_str());

  // Load configuration from EEPROM before setting up display
  eepromLoadCompanionConfig();

  // Matrix init
  P.begin();
  P.setIntensity(8);
  P.displayClear();

  // Set initial brightness immediately after matrix init (now with loaded value)
  setMatrixBrightnessFromPercent(brightness);

  // Grab underlying MAX72XX object
  mx = P.getGraphicObject();

  // Define a single zone 0 spanning all devices
  P.setZone(0, 0, MAX_DEVICES - 1);
  // Flip left/right to match your 4,3,2,1 wiring so it reads 1-2-3-4
  P.setZoneEffect(0, true, PA_FLIP_LR);

  // Ensure bars are off at boot
  updateBackgroundBars(false, false);

  StartupAction startupAction = eepromReadStartupAction();
  eepromWriteStartupAction(STARTUP_NORMAL);  // consume one-time menu action

  if (startupAction == STARTUP_WIFI_AP) {
    Serial.println("[Setup] Opening WiFi AP from setup menu");
    startConfigPortal();
    ESP.restart();
  }

  // Show "Hello!" for 3 seconds
  showBootMessage("Hello!");
  
  unsigned long helloStart = millis();
  while (millis() - helloStart < 3000) {
    if (P.displayAnimate()) {
      if (textScrolls) {
        P.displayReset();
      }
    }
    yield();  // keep the watchdog happy
  }
  
  // Clear screen after Hello! message completes
  P.displayClear();
  currentText = "";
  
  // WiFi + config
  connectToNetwork();

  // The REST API must be ready before we advertise the service: Companion's
  // auto-setup immediately POSTs the selected device's Companion host/port.
  setupRestAPI();
  setupMDNS();

  if (startupAction == STARTUP_WEB_CONFIG) {
    startWebConfigPortal();
  }

  Serial.println("[System] Setup complete, entering loop");
}

// ------------------------------------------------------------
// LOOP
// ------------------------------------------------------------

void loop() {
  unsigned long now = millis();

  handleDownloadButton(now);

  // Handle mDNS updates only after a successful responder setup.
  if (mdnsStarted) MDNS.update();

  // Handle REST API requests
  restServer.handleClient();

  // Attempt Companion connection if not connected
  if (!client.connected() && (now - lastConnectTry >= connectRetryMs)) {
    lastConnectTry = now;
    
    // Mark that first connection attempt has been made
    firstConnectionAttemptMade = true;

    Serial.print("[NET] Connecting to Companion ");
    Serial.print(companion_host);
    Serial.print(":");
    Serial.println(companion_port);

    // Set connection timeout to ensure non-blocking behavior
    client.setTimeout(1000);  // 1 second timeout
    
    if (client.connect(companion_host, atoi(companion_port))) {
      Serial.println("[NET] Connected to Companion API");
      // WiFi ! remains displayed untilsetText("Connecting...") overwrites it
      sendAddDevice();
      lastPingTime = millis();
      // Start timer for delayed Connecting... message (only if TCP connection succeeded)
      connectionStartTime = now;
      connectingMessageShown = false;
      connectionEstablished = false; // Reset for new connection
    } else {
      Serial.println("[NET] Companion connect failed");
      // Reset connection timer on failure - don't show Connecting... message
      connectionStartTime = 0;
      connectingMessageShown = false;
      connectionEstablished = false;
      // NO config portal here – just keep retrying
    }
  }

  // Handle delayed Connecting... message display (only if TCP connection succeeded and not established)
  if (client.connected() && !connectionEstablished && !connectingMessageShown && connectionStartTime > 0 && 
      (now - connectionStartTime >= connectingMessageDelay)) {
    setText("Connecting...");
    connectingMessageShown = true;
    Serial.println("[Display] Showing Connecting... after 1s delay");
  }

  // Handle Companion traffic
  if (client.connected()) {
    while (client.available()) {
      String line = client.readStringUntil('\n');
      line.trim();
      if (line.length() > 0) {
        // Mark connection as established and stop showing Connecting...
        if (!connectionEstablished) {
          connectionEstablished = true;
          connectingMessageShown = false;
          Serial.println("[Display] Connection established - stopping Connecting...");
          // Clear screen before first Companion update to avoid flickering
          P.displayClear();
        }
        parseAPI(line);
      }
    }

    // Periodic PING
    if (now - lastPingTime >= pingIntervalMs) {
      client.println("PING ledmatrix");
      lastPingTime = now;
    }
  }

  // Handle delayed brightness write (debounce) - runs regardless of connection status
  if (brightnessWritePending) {
    // Use overflow-safe time comparison
    if ((long)(now - lastBrightnessChange) >= (long)brightnessWriteDelay) {
      Serial.printf("[DEBUG] Brightness write after %lu ms delay\n", brightnessWriteDelay);
      
      // Check if brightness actually changed from EEPROM value to prevent unnecessary writes
      EEPROM.begin(EEPROM_SIZE);
      uint8_t storedBrightness = EEPROM.read(66);
      EEPROM.end();
      
      if (storedBrightness != (uint8_t)brightness) {
        eepromSaveCompanionConfig(companion_host, companion_port);
        Serial.println("[EEPROM] Brightness written to EEPROM after debounce delay (value changed)");
      } else {
        Serial.println("[EEPROM] Brightness not written - value unchanged in EEPROM");
      }
      brightnessWritePending = false;
    }
  }

  // Handle WiFi ! / MAC address alternation
  // Only alternate when Companion is NOT connected AND first connection attempt has been made
  // AND we're in alternation mode (either showing WiFi ! or MAC)
  if (!client.connected() && firstConnectionAttemptMade && 
      (currentText == "WiFi !" || currentText == macShort)) {
    
    // If we just switched to WiFi ! (from MAC), reset the timer
    if (showMACAddress && currentText == "WiFi !") {
      lastWiFiOKToggle = now;
    }
    
    // If we just switched to MAC (from WiFi !), reset the timer
    if (!showMACAddress && currentText == macShort) {
      lastWiFiOKToggle = now;
    }
    
    if ((long)(now - lastWiFiOKToggle) >= (long)wifiOKToggleDelay) {
      lastWiFiOKToggle = now;
      showMACAddress = !showMACAddress;
      
      if (showMACAddress) {
        setTextNow(macShort);
      } else {
        setTextNow("WiFi !");
      }
    }
  } else {
    // Reset alternation when not showing WiFi ! or MAC or when Companion is connected
    showMACAddress = false;
    lastWiFiOKToggle = now;
  }

  // Apply any pending text changes
  if (textDirty) {
    textDirty = false;
    setTextNow(pendingText);
  }

  // Let Parola do its thing
  if (P.displayAnimate()) {
    // Only auto-reset for scrolling animations.
    // For static text (PRINT), leave it alone so it doesn’t flash.
    if (textScrolls) {
      P.displayReset();
    }

    // Re-apply bars each animation step in bar modes - only when Companion is connected
    if (client.connected() && (bgMode == BG_BARS || bgMode == BG_PGMPVW || bgMode == BG_PVWPGM)) {
      updateBackgroundBars(barLeftOn, barRightOn);
    }
  }
}
