# LED Matrix Companion v4 Satellite (ESP8266)

ESP8266-based LED matrix “satellite” for **Bitfocus Companion v4**.

This firmware connects to Companion’s **Satellite API**, subscribes to key text, and renders it on a **32×8 LED matrix** (4× MAX7219 8×8 modules in a row).

Buy the units here: https://www.aliexpress.com/item/1005006038630745.html

- ✅ Supports **TEXT from Companion** (base64 decoded)
- ✅ Centers short text, scrolls long text
- ✅ **Brightness** control via Companion BRIGHTNESS (0–100 → 0–15 LED intensity)
- ✅ **WiFiManager** config portal (for WiFi + Companion IP/port)
- ✅ Companion IP/port stored in **EEPROM**
- ✅ **Boot counter** to force config mode with repeated resets
- ✅ Designed for small custom PCBs or dev boards (e.g. Wemos D1 Mini) with MAX7219 chain

---

## Hardware

https://www.aliexpress.com/item/1005006038630745.html


## Connecting to Wi-Fi / Companion

The device boots and attempts Wi-Fi and Companion automatically.

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

##Companion Setup

In Companion v4:
- It should automatically show up as Satellite Surface
- Assign the button offset in Surfaces

## The device appears as:

PRODUCT_NAME = "LED Matrix"
KEYS_TOTAL   = 1
TEXT         = true

# You can now push text via:
Buttons
which means it can be driven by: Variables & Custom Actions!

## Display Behaviour
- Text ≤5 chars → centred static
- Long text → smooth scroll from right to left
- Updates do not flicker
- BRIGHTNESS 0–100 maps to matrix intensity 0–15
- “Waiting…” appears until Companion connects

## Reset Modes
### Normal reset
- Tap RESET — device reconnects immediately.
- Enter flash mode
- Hold DOWNLOAD → tap RESET → release.

### Enter config portal
- Either:
  - Hold DOWNLOAD during power-on, or
  - Allow 5 failed Wi-Fi attempts in a row.
