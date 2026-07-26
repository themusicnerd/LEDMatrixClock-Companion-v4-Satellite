# LED Matrix Companion v4 Satellite (ESP8266)

ESP8266-based LED matrix “satellite” for **Bitfocus Companion v4**.

This firmware connects to Companion’s **Satellite API**, subscribes to key text, and renders it on a **32×8 LED matrix** (4× MAX7219 8×8 modules in a row).

Buy the units here: https://www.aliexpress.com/item/1005006038630745.html

- ✅ Supports **TEXT from Companion** (base64 decoded)
- ✅ Centers short text, scrolls long text
- ✅ COLOR parsing from Companion (`COLOR="#RRGGBB"` or `R,G,B`)
- ✅ Background Modes (`bgmode`):
  - `none`   – ignore COLOR, no invert, no bars
  - `invert` – invert full display when any channel ≥ 128
  - `bars`   – left & right 2-column bars ON when any channel ≥ 128
  - `pgmpvw` – left bars = PGM (red ≥ 128), right bars = PVW (green ≥ 128)
  - `pvwpgm` – left bars = PVW (green ≥ 128), right bars = PGM (red ≥ 128)
- ✅ **Brightness** control via Companion BRIGHTNESS (0–100 → 0–15 LED intensity)
- ✅ **WiFiManager** config portal (for WiFi + Companion IP/port + bgmode)
- ✅ Companion IP/port + background mode stored in **EEPROM**
- ✅ Companion auto-discovery through `_companion-satellite._tcp` mDNS, including one-click REST setup
- ✅ AtomS3-style setup menu via a two-second **DOWNLOAD** button hold after boot
- ✅ Designed for cheap LED matrix clocks based on ESP8266 from AliExpress

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

## Arduino IDE Setup

Before uploading the firmware, you need to install the ESP8266 board support in Arduino IDE:

1. Start Arduino and open the Preferences window
2. Enter `https://arduino.esp8266.com/stable/package_esp8266com_index.json` into the **File > Preferences > Additional Boards Manager URLs** field of the Arduino IDE. You can add multiple URLs, separating them with commas.
3. Open **Boards Manager** from **Tools > Board** menu and install **esp8266** platform (and don't forget to select your ESP8266 board from **Tools > Board** menu after installation).

## Required Libraries

Install the following libraries via **Sketch > Include Library > Manage Libraries**:

1. **WiFiManager** by tzapu
2. **MD_Parola** by MajicDesigns
3. **MD_MAX72XX** by MajicDesigns

After installing these libraries, you should be able to compile and upload the firmware.

### Required mDNS core patch

Companion discovers satellites via `_companion-satellite._tcp`. The stock ESP8266 mDNS core limits service names to 15 characters, while `companion-satellite` is 19 characters. Apply [patches/esp8266-mdns-service-name-length.patch](patches/esp8266-mdns-service-name-length.patch) to `libraries/ESP8266mDNS/src/LEAmDNS.h` in the installed ESP8266 board package, then rebuild/upload. The firmware emits a compile warning and a serial error if that patch is missing.

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

After a normal boot, hold the **DOWNLOAD** button for two seconds to open the setup menu. Short-press to cycle the selection and hold DOWNLOAD for two seconds to select it:

- `NORMAL` — exit the menu and continue running.
- `WEB CFG` — open WiFiManager’s web configuration portal on the current Wi-Fi network; hold DOWNLOAD for two seconds to close it and reboot.
- `WIFI AP` — start the `led-matrix_XXXXX` Wi-Fi configuration access point.
- `RESET` — factory reset saved Wi-Fi, Companion, and display settings. This requires a second confirmation: short-press to change `NO RESET` to `YES RESET`, then hold DOWNLOAD for two seconds.

Do **not** hold DOWNLOAD while pressing RESET: it is connected to GPIO0 and will put the ESP8266 into the serial flashing bootloader instead of running the firmware.

The original reset-count fallback remains available: reset the device while `CONFIG?` is scrolling, then it will enter WiFi Manager config mode on the next boot and display `CFG !`.
- Connect to LED-MATRIX-XXXXXXXXXXX (with the mac address at the end)
- It should take you to 192.168.4.1 (if not, go there)
- Setup the Companion IP, Companion Port, set the mode you want, and set Boot to 0
- Modes Are:
- - none    – ignore COLOR, no invert, no bars
- - invert  – invert the whole display when any color channel ≥ 128
- - bars    – left and right 2-column bars ON when any color channel ≥ 128
- - pgmpvw  – left = PGM (red ≥ 128), right = PVW (green ≥ 128)
- - pvwpgm  – left = PVW (green ≥ 128), right = PGM (red ≥ 128)
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
- Select the discovered device and let Companion configure its own host/port through the satellite REST API. Manual Companion IP/port configuration remains available as a fallback.

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

## Browser Firmware Update

1. Download the **ESP8266MOD 4 MB** `LEDMatrixClock-Companion-v4-Satellite.ino.bin` from a GitHub release.
2. Browse to `http://<device-ip>:9999/update` while connected to the same network.
3. Updates are open by default. Use the optional protection form on that page to set a password; once set, sign in as `admin` with that password.
4. Select the `.bin`, upload it, and wait for the automatic reboot. Do not remove power during the update.

Upload only the release application `.bin`; serial flash images and files made for a different flash layout are not suitable for browser updates.

## Companion discovery and setup menu

The satellite advertises `companion-satellite._tcp` with AtomS3-compatible metadata, so Companion can discover and configure it automatically. Hold **DOWNLOAD** for two seconds after boot to open the setup menu; choose normal boot, Web Config, Wi-Fi AP, or Factory Reset. The display modes are `none`, `invert`, `bars`, `pgmpvw`, and `pvwpgm`.
