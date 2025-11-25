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
    - 5 rapid resets before successful WiFi connection =>
      boot counter triggers config portal
  ------------------------------------------------------------
*/

#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <EEPROM.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>

// ------------------------ MATRIX CONFIG ---------------------

#define HARDWARE_TYPE MD_MAX72XX::PAROLA_HW
#define MAX_DEVICES   4

// Pins for ESP8266 (numeric GPIOs)
#define PIN_CS   15    // D8 on many dev boards
#define PIN_CLK  14    // D5
#define PIN_DIN  13    // D7

MD_Parola P = MD_Parola(HARDWARE_TYPE, PIN_CS, MAX_DEVICES);

// Global text buffer that Parola will use (must stay valid!)
char matrixText[96];   // adjust size if you want longer max text

// Track whether current text is scrolling or static
bool textScrolls = false;

// Parola timing
const uint16_t scrollSpeed = 40;     // Lower = slower
const uint16_t scrollPause = 0;      // Pause at end of scroll

bool testMode = true;   // start in test mode for a few seconds
unsigned long testStart = 0;

// ------------------------ COMPANION CONFIG ------------------

WiFiManager wifiManager;
WiFiClient  client;

// What we store in EEPROM
char companion_host[40] = "Companion IP";
char companion_port[6]  = "16622";

// WiFiManager custom params
WiFiManagerParameter* custom_companionIP;
WiFiManagerParameter* custom_companionPort;

// Device ID and hostname
String deviceID;

// AP password for config portal (blank = open)
const char* AP_password = "";

// EEPROM layout
// [0] = 'L', [1] = 'M' magic
// [2] = version
// [3..42]  = companion_host (40 bytes)
// [43..48] = companion_port (6 bytes)
// [60]     = bootCounter
const uint16_t EEPROM_SIZE      = 128;
const uint16_t EEPROM_BOOT_ADDR = 60;

// Timing / connection
unsigned long lastPingTime     = 0;
unsigned long lastConnectTry   = 0;
const unsigned long connectRetryMs  = 5000;
const unsigned long pingIntervalMs  = 2000;

// How many failed boots before forcing config portal
const uint8_t BOOT_FAIL_LIMIT = 5;

// Brightness (0–100 from Companion)
int brightness = 100;

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
  }
  EEPROM.end();
}

void eepromSaveCompanionConfig(const char* host, const char* port) {
  EEPROM.begin(EEPROM_SIZE);

  EEPROM.write(0, 'L');
  EEPROM.write(1, 'M');
  EEPROM.write(2, 1); // version

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

  if (str_companionIP.length() > 0) {
    str_companionIP.toCharArray(companion_host, sizeof(companion_host));
  }
  if (str_companionPort.length() > 0) {
    str_companionPort.toCharArray(companion_port, sizeof(companion_port));
  }

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
// Companion / Satellite API parsing
// ------------------------------------------------------------

void sendAddDevice() {
  String cmd = "ADD-DEVICE DEVICEID=" + deviceID +
               " PRODUCT_NAME=\"LED Matrix\" KEYS_TOTAL=1 KEYS_PER_ROW=1 BITMAPS=0 COLORS=0 TEXT=true";
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
    handleKeyStateText(apiData);
    return;
  }
}

// ------------------------------------------------------------
// CONFIG PORTAL (explicit trigger from boot counter)
// ------------------------------------------------------------
void startConfigPortal() {
  Serial.println("[WiFi] Entering CONFIG PORTAL mode due to boot counter");
  showBootMessage("CONFIG\n192.168.4.1");

  // No timeout when we explicitly call config mode
  wifiManager.setConfigPortalTimeout(0);

  // Start AP + portal, blocks until user saves or exits
  wifiManager.startConfigPortal(deviceID.c_str(), AP_password);

  // After returning, update our Companion host/port and persist
  strncpy(companion_host, custom_companionIP->getValue(), sizeof(companion_host));
  companion_host[sizeof(companion_host) - 1] = '\0';

  strncpy(companion_port, custom_companionPort->getValue(), sizeof(companion_port));
  companion_port[sizeof(companion_port) - 1] = '\0';

  eepromSaveCompanionConfig(companion_host, companion_port);

  // Reset boot counter so we do not immediately re-enter config
  eepromWriteBootCounter(0);

  // Show a small message so you know it applied
  showBootMessage("CFG SAVED");
  delay(1000);
}

// ------------------------------------------------------------
// WiFi / Initial Config + Boot Counter logic
// ------------------------------------------------------------

void connectToNetwork() {
  WiFi.mode(WIFI_STA);

  // Load Companion config from EEPROM
  eepromLoadCompanionConfig();

  // Increment boot counter as early as possible
  uint8_t bootCount = eepromReadBootCounter();
  if (bootCount < 255) {
    bootCount++;
  }
  eepromWriteBootCounter(bootCount);
  Serial.printf("[Boot] Boot counter = %u\n", bootCount);

  // Prepare WiFiManager with params
  custom_companionIP   = new WiFiManagerParameter("companionIP", "Companion IP", companion_host, 40);
  custom_companionPort = new WiFiManagerParameter("companionPort", "Satellite Port", companion_port, 6);

  wifiManager.addParameter(custom_companionIP);
  wifiManager.addParameter(custom_companionPort);
  wifiManager.setSaveParamsCallback(saveParamCallback);

  std::vector<const char*> menu = { "wifi", "param", "info", "sep", "restart", "exit" };
  wifiManager.setMenu(menu);
  wifiManager.setClass("invert");
  wifiManager.setConfigPortalTimeout(180); // 3 minutes auto portal if WiFi fails

  wifiManager.setAPCallback([](WiFiManager* wm) {
    Serial.println("[WiFi] Config portal started");
    showBootMessage("CONFIG\n192.168.4.1");
  });

  // If we see too many early resets, force config portal
  if (bootCount >= BOOT_FAIL_LIMIT) {
    startConfigPortal();
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

  // Copy latest values
  strncpy(companion_host, custom_companionIP->getValue(), sizeof(companion_host));
  companion_host[sizeof(companion_host) - 1] = '\0';

  strncpy(companion_port, custom_companionPort->getValue(), sizeof(companion_port));
  companion_port[sizeof(companion_port) - 1] = '\0';

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

  // Define a single zone 0 spanning all devices
  P.setZone(0, 0, MAX_DEVICES - 1);
  // Flip left/right to match your 4,3,2,1 wiring so it reads 1-2-3-4
  P.setZoneEffect(0, true, PA_FLIP_LR);
  // If it ever looks upside-down, you can also try:
  // P.setZoneEffect(0, true, PA_FLIP_UD);

  // Quick visual tests
  P.displayText("48:00", PA_CENTER, scrollSpeed, scrollPause, PA_PRINT, PA_PRINT);
  P.displayReset();
  delay(3000);

  P.displayText("1234", PA_CENTER, scrollSpeed, scrollPause, PA_PRINT, PA_PRINT);
  P.displayReset();

  testStart = millis();
  testMode  = true;

  showBootMessage("BOOTING\nLED MATRIX");

  // WiFi + config (with boot counter logic)
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

  // --- TEST MODE: keep simple text for 5 seconds on boot ---
  if (testMode) {
    if (P.displayAnimate()) {
      P.displayReset();   // keep it static/looping
    }
    if (now - testStart > 5000) {
      testMode = false;
      P.displayClear();
    }
    return;   // don't run the Companion logic while in test mode
  }

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
      delay(200);
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
    // For static text (PRINT), leave it alone so it doesn't flash.
    if (textScrolls) {
      P.displayReset();
    }
  }
}
