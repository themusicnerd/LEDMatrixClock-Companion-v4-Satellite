# LED Matrix Companion v4 Satellite (ESP8266)

ESP8266-based LED matrix “satellite” for **Bitfocus Companion v4**.

This firmware connects to Companion’s **Satellite API**, subscribes to key text, and renders it on a **32×8 LED matrix** (4× MAX7219 8×8 modules in a row).

Buy the units here: https://www.aliexpress.com/item/1005006038630745.html

- ✅ Supports **TEXT from Companion** (base64 decoded)
- ✅ Centers short text, scrolls long text
- ✅ COLOR parsing from Companion (COLOR="#RRGGBB" or R,G,B)
- ✅ Auto-invert display when background color is bright
- - If R ≥ 128 or G ≥ 128 or B ≥ 128 → invert = ON
- - Keeps text readable regardless of Companion background color
- ✅ **Brightness** control via Companion BRIGHTNESS (0–100 → 0–15 LED intensity)
- ✅ **WiFiManager** config portal (for WiFi + Companion IP/port)
- ✅ Companion IP/port stored in **EEPROM**
- ✅ **Boot counter** to force config mode with repeated resets
- ✅ Designed for cheap LED Matrix clocks based of ESP8266 on AliExpress

---
# Display Behaviour

## Static text
- Text ≤ 5 characters → centered
- Text width ≤ 32 px → centered

## Scrolling text
- Text wider than 32 columns → smooth right-to-left scroll
- Scroll restarts only when text changes (no flashing)

## Rendering

- Zero flicker on updates
- Full UTF-8 via decoded base64 TEXT
- Automatic polarity switching via Companion color
- “Waiting…” shown until Companion responds

---

## Hardware

https://www.aliexpress.com/item/1005006038630745.html

## Connecting to Wi-Fi / Companion

The device boots and attempts Wi-Fi and Companion automatically.

## WiFiManager Powered

- Built-in captive portal for:
- - Wi-Fi SSID & Password
- - Companion IP
- - Companion Port
- - Boot Counter info
- Stores Companion info in EEPROM
- - Automatically reconnects to both Wi-Fi and Companion

## Automatic Boot-Recovery

- During boot, device displays CONFIG? for 5 seconds
- If you reset the device during this window → next boot forces config mode
- Boot counter stored in EEPROM ensures this works reliably
- You will NOT go into config mode just because Companion is offline (device keeps retrying forever)

## Entering Config Portal

On boot if you see CONFIG? and you reset while it is scrolling, you will enter WiFi Manager config mode.
- Connect to LED-MATRIX-XXXXXXXXXXX (with the mac address at the end)
- It should take you to 192.168.4.1 (if not, go there)
- Setup the Companion IP, Companion Port, and set Boot to 0
- Save
- Hit BACK in your browser to get to the main menu (may be more than once)
- Setup Wifi
- Save and exit

## Connect to the Wi-Fi network:

SSID: LEDMatrix-<MAC>
Password: (blank)


Browse to:
http://192.168.4.1


Configure:
- Wi-Fi SSID & password
- Companion IP
- Companion Satellite Port (default 16622)
Settings save to EEPROM.

## Companion Setup

In Companion v4:
- It should automatically show up as Satellite Surface
- Assign the button offset in Surfaces

## The device appears as:

PRODUCT_NAME = LED Matrix
KEYS_TOTAL   = 1
TEXT         = true
COLORS       = true

# You can now push text via:
Buttons
which means it can be driven by: Variables & Custom Actions!

## Display Behaviour
- Text ≤5 chars → centred static
- Long text → smooth scroll from right to left
- Updates do not flicker
- BRIGHTNESS 0–100 maps to matrix intensity 0–15
- “Waiting…” appears until Companion connects
