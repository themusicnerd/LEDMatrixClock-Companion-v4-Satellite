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
    - Reset during 5s "CONFIG?" window =>
      next boot triggers config portal via boot counter

  Background handling modes (configurable via WiFiManager):
    - none    : ignore COLOR, no invert, no bars
    - invert  : invert the whole display based on COLOR
    - bars    : left+right 2 columns ON if any channel >=128
    - pgmpvw  : left 2 = red>=128 (PGM), right 2 = green>=128 (PVW)
    - pvwpgm  : left 2 = green>=128 (PVW), right 2 = red>=128 (PGM)
  ------------------------------------------------------------
*/

#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <EEPROM.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <vector>

// ------------------------ MATRIX CONFIG ---------------------

#define HARDWARE_TYPE MD_MAX72XX::PAROLA_HW
#define MAX_DEVICES   4

// Pins for ESP8266 (numeric GPIOs)
#define PIN_CS   15    // D8 on many dev boards
#define PIN_CLK  14    // D5
#define PIN_DIN  13    // D7

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

// What we store in EEPROM
char companion_host[40] = "Companion IP";
char companion_port[6]  = "16622";

// WiFiManager custom params
WiFiManagerParameter* custom_companionIP;
WiFiManagerParameter* custom_companionPort;
WiFiManagerParameter* param_bootCount;   // info/debug param
WiFiManagerParameter* custom_bgMode;     // background handling mode

// Device ID and hostname
String deviceID;

// AP password for config portal (blank = open)
const char* AP_password = "";

// EEPROM layout
// [0] = 'L', [1] = 'M' magic
// [2] = version
// [3..42]  = companion_host (40 bytes)
// [43..48] = companion_port (6 bytes)
// [50..65] = bg_mode_str (16 bytes)
// [60]     = bootCounter
const uint16_t EEPROM_SIZE      = 128;
const uint16_t EEPROM_BOOT_ADDR = 60;

// Timing / connection
unsigned long lastPingTime     = 0;
unsigned long lastConnectTry   = 0;
const unsigned long connectRetryMs  = 5000;
const unsigned long pingIntervalMs  = 2000;

// How many previous boots before forcing config portal
const uint8_t BOOT_FAIL_LIMIT = 1;

// Cached copy of PRE-INCREMENT boot counter (from EEPROM)
uint8_t bootCountCached = 0;

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
void eepromLoadCompanionConfig() {
  EEPROM.begin(EEPROM_SIZE);
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

    // background mode string (16 bytes)
    for (int i = 0; i < 16; i++) {
      bg_mode_str[i] = (char)EEPROM.read(50 + i);
    }
    bg_mode_str[15] = '\0';

    // If uninitialised (0xFF or empty), default to "invert"
    if (bg_mode_str[0] == (char)0xFF || bg_mode_str[0] == '\0') {
      strncpy(bg_mode_str, "invert", sizeof(bg_mode_str));
      bg_mode_str[sizeof(bg_mode_str) - 1] = '\0';
    }
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

  // background mode string
  for (int i = 0; i < 16; i++) {
    char c = (i < (int)strlen(bg_mode_str)) ? bg_mode_str[i] : 0;
    EEPROM.write(50 + i, (uint8_t)c);
  }

  EEPROM.commit();
  EEPROM.end();
}

uint8_t eepromReadBootCounter() {
  EEPROM.begin(EEPROM_SIZE);
  uint8_t c = EEPROM.read(EEPROM_BOOT_ADDR);
  EEPROM.end();
  return c;
}

void eepromWriteBootCounter(uint8_t c) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(EEPROM_BOOT_ADDR, c);
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
  String str_bootCount     = getParam("bootCount");
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

  // Optional: allow user to override boot counter from portal
  if (str_bootCount.length() > 0) {
    uint8_t newBC = (uint8_t)str_bootCount.toInt();
    eepromWriteBootCounter(newBC);
    Serial.printf("[WiFi] Boot counter updated from portal: %u\n", newBC);
  }

  // Update bgMode from updated string
  bgMode = parseBgMode(bg_mode_str);
  applyBackgroundFromLastColor();

  eepromSaveCompanionConfig(companion_host, companion_port);
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

  // Reapply bars on top of whatever we just drew (modes that use bars)
  if (bgMode == BG_BARS || bgMode == BG_PGMPVW || bgMode == BG_PVWPGM) {
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
  String cmd = "ADD-DEVICE DEVICEID=" + deviceID +
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
  P.print("CFG!");
  textScrolls = false;
  currentText = "CFG!";

  // Clear bars in config mode (just show text)
  updateBackgroundBars(false, false);
}

void startConfigPortal() {
  Serial.println("[WiFi] Entering CONFIG PORTAL mode (boot counter)");
  showConfigModeMessage();

  // No timeout when we explicitly call config mode
  wifiManager.setConfigPortalTimeout(0);

  // Start AP + portal, blocks until user saves or exits
  wifiManager.startConfigPortal(deviceID.c_str(), AP_password);

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

  // Reset boot counter so we do not immediately re-enter config
  eepromWriteBootCounter(0);

  // Show a small message so you know it applied
  showBootMessage("CFG SAVED");
  delay(1000);
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

  Serial.printf("[Boot] Cached boot counter (prev boot) = %u\n", bootCountCached);

  // ---------- Prepare WiFiManager with params BEFORE any portal ----------

  // Companion IP / Port params
  custom_companionIP   = new WiFiManagerParameter("companionIP", "Companion IP", companion_host, 40);
  custom_companionPort = new WiFiManagerParameter("companionPort", "Satellite Port", companion_port, 6);

  // Boot counter info param
  char bcStr[6];
  snprintf(bcStr, sizeof(bcStr), "%u", bootCountCached);
  param_bootCount = new WiFiManagerParameter("bootCount", "Boot Counter (set to 0 to boot normally)", bcStr, 5);

  // Background mode param
  custom_bgMode = new WiFiManagerParameter(
    "bgmode",
    "Background Handling (none/invert/bars/pgmpvw/pvwpgm)",
    bg_mode_str,
    sizeof(bg_mode_str)
  );

  wifiManager.addParameter(custom_companionIP);
  wifiManager.addParameter(custom_companionPort);
  wifiManager.addParameter(param_bootCount);
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
  // If previous boot requested config (via reset during CONFIG? window)
  // -----------------------------------------------------------------------
  if (bootCountCached >= BOOT_FAIL_LIMIT) {
    startConfigPortal();
    // startConfigPortal() resets boot counter to 0 on success
  }

  // Normal autoConnect behaviour (connect to WiFi, or start portal if no WiFi)
  bool res = wifiManager.autoConnect(deviceID.c_str(), AP_password);

  if (!res) {
    Serial.println("[WiFi] Failed to connect, restarting...");
    showBootMessage("WiFi ERR");
    delay(1000);
    ESP.restart();
  } else {
    Serial.print("[WiFi] Connected: ");
    Serial.println(WiFi.localIP());
    showBootMessage("WiFi OK");
    delay(1000);
  }

  // Copy latest values (including bgMode)
  strncpy(companion_host, custom_companionIP->getValue(), sizeof(companion_host));
  companion_host[sizeof(companion_host) - 1] = '\0';

  strncpy(companion_port, custom_companionPort->getValue(), sizeof(companion_port));
  companion_port[sizeof(companion_port) - 1] = '\0';

  strncpy(bg_mode_str, custom_bgMode->getValue(), sizeof(bg_mode_str));
  bg_mode_str[sizeof(bg_mode_str) - 1] = '\0';
  bgMode = parseBgMode(bg_mode_str);
  applyBackgroundFromLastColor();

  eepromSaveCompanionConfig(companion_host, companion_port);

  // WiFi successfully connected => clear boot counter
  eepromWriteBootCounter(0);
  Serial.println("[Boot] Boot counter reset to 0 (WiFi OK)");
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

  uint8_t mac[6];
  WiFi.macAddress(mac);

  char macBuf[13];
  sprintf(macBuf, "%02X%02X%02X%02X%02X%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  deviceID  = "LED_Matrix_";
  deviceID += macBuf;
  deviceID.toUpperCase();

  Serial.println("[ID] deviceID = " + deviceID);
  WiFi.hostname(deviceID);

  // Matrix init
  P.begin();
  P.setIntensity(8);
  P.displayClear();

  // Grab underlying MAX72XX object
  mx = P.getGraphicObject();

  // Define a single zone 0 spanning all devices
  P.setZone(0, 0, MAX_DEVICES - 1);
  // Flip left/right to match your 4,3,2,1 wiring so it reads 1-2-3-4
  P.setZoneEffect(0, true, PA_FLIP_LR);

  // Ensure bars are off at boot
  updateBackgroundBars(false, false);

  // --------------------------------------------------------
  // Boot counter logic:
  //  - bootCountCached = value from PREVIOUS run
  //  - We immediately bump stored value so that if we reset
  //    during CONFIG? window, next boot sees non-zero.
  // --------------------------------------------------------
  bootCountCached = eepromReadBootCounter();
  uint8_t newCount = bootCountCached;
  if (newCount < 255) {
    newCount++;
  }
  eepromWriteBootCounter(newCount);
  Serial.printf("[Boot] Boot counter previous=%u, new=%u\n", bootCountCached, newCount);

  // Show "BOOT" for 1 second
  showBootMessage("BOOT");
  delay(1000);

  // Show "CONFIG?" and sit there for CONFIG_PROMPT_MS.
  showBootMessage("CONFIG?");
  unsigned long cfgPromptStart = millis();
  while (millis() - cfgPromptStart < CONFIG_PROMPT_MS) {
    if (P.displayAnimate()) {
      if (textScrolls) {
        P.displayReset();
      }
    }
    yield();  // keep the watchdog happy
  }

  // WiFi + config (with boot counter logic via bootCountCached)
  connectToNetwork();

  // After WiFi, show IP + Companion IP briefly
  String ipMsg = "IP " + WiFi.localIP().toString();
  showBootMessage(ipMsg);
  delay(2000);   // a bit more time so you can read it

  String compMsg = "COMP " + String(companion_host);
  showBootMessage(compMsg);
  delay(2000);

  // Set initial brightness
  setMatrixBrightnessFromPercent(brightness);

  Serial.println("[System] Setup complete, entering loop");
}

// ------------------------------------------------------------
// LOOP
// ------------------------------------------------------------

void loop() {
  unsigned long now = millis();

  // Attempt Companion connection if not connected
  if (!client.connected() && (now - lastConnectTry >= connectRetryMs)) {
    lastConnectTry = now;

    Serial.print("[NET] Connecting to Companion ");
    Serial.print(companion_host);
    Serial.print(":");
    Serial.println(companion_port);

    if (client.connect(companion_host, atoi(companion_port))) {
      Serial.println("[NET] Connected to Companion API");
      showBootMessage("Connected");
      delay(500);
      sendAddDevice();
      lastPingTime = millis();
      // Show waiting text until Companion sends first TEXT
      setText("Waiting...");
    } else {
      Serial.println("[NET] Companion connect failed");
      // NO config portal here – just keep retrying
    }
  }

  // Handle Companion traffic
  if (client.connected()) {
    while (client.available()) {
      String line = client.readStringUntil('\n');
      line.trim();
      if (line.length() > 0) {
        parseAPI(line);
      }
    }

    // Periodic PING
    if (now - lastPingTime >= pingIntervalMs) {
      client.println("PING ledmatrix");
      lastPingTime = now;
    }
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

    // Re-apply bars each animation step in bar modes
    if (bgMode == BG_BARS || bgMode == BG_PGMPVW || bgMode == BG_PVWPGM) {
      updateBackgroundBars(barLeftOn, barRightOn);
    }
  }
}
