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
- **MCU**: ESP8266 (e.g. Wemos D1 mini, NodeMCU, or custom ESP8266MOD board)
- **Display**: 32×8 LED matrix using **4× MAX7219** modules in a chain
- **Wiring** (ESP8266 GPIO):

| Signal | ESP8266 GPIO | Typical dev-board pin |
|--------|--------------|------------------------|
| CS     | 15           | D8                    |
| CLK    | 14           | D5                    |
| DIN    | 13           | D7                    |

- The firmware is configured for:

```cpp
#define HARDWARE_TYPE MD_MAX72XX::PAROLA_HW
#define MAX_DEVICES   4
