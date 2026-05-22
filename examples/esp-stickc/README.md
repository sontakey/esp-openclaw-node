# M5StickC Plus2 Display Example

This example runs an M5StickC Plus2 as an OpenClaw Node with display commands for the built-in 1.14" TFT screen.

## Hardware

| Component | Details |
|-----------|---------|
| MCU | ESP32-PICO-V3-02 |
| Display | 1.14" TFT, 135x240, ST7789v2 over SPI |
| Power latch | GPIO4 (must be held HIGH for battery operation) |

### LCD Pin Map

| Function | GPIO |
|----------|------|
| SCLK | 13 |
| MOSI | 15 |
| CS | 5 |
| DC | 14 |
| RST | 12 |
| Backlight | 27 |

### Blank-Screen Fixes

The M5StickC Plus2 display can appear blank for three common reasons, all of which this example handles explicitly:

1. **Backlight not enabled.** GPIO27 must be driven HIGH; the panel data is sent correctly but the backlight stays dark if this pin is left low. This example calls `gpio_set_level(GPIO_NUM_27, 1)` during initialisation.

2. **Wrong ST7789 offset.** The 135x240 visible window is centred inside the ST7789's native 240x320 framebuffer. The required gap offsets are **x_gap = 52** and **y_gap = 40** in portrait (default MADCTL rotation). Without these offsets, content is rendered off-screen. This example passes the offsets via `esp_lcd_panel_set_gap()`.

3. **Power latch.** GPIO4 must be held HIGH so the device stays powered when running on battery. This example configures GPIO4 as output and sets it HIGH at startup, before any other peripheral init.

## What This Example Exposes

- `device`
- `wifi`
- `display`

Commands:

- `device.info`
- `device.status`
- `wifi.status`
- `display.show`
- `display.status`

## Display Payload

`display.show` accepts:

- `heading` short title text, up to `64` UTF-8 bytes
- `text` body text, up to `512` UTF-8 bytes

Sample payload:

```json
{
  "heading": "Hello",
  "text": "OpenClaw is driving the M5StickC Plus2 display."
}
```

On boot, the body area shows a waiting message until the node is paired.

## Status Footer

The bottom of the screen always shows a live status footer that the device
renders on its own, with no gateway interaction required:

- **Wi-Fi** the connected SSID, or `offline`
- the device IPv4 address and current signal strength in dBm
- **Gateway** the OpenClaw session state: `connecting`, `connected`, or `offline`
- **Node** the short device id (use `openclaw nodes status --json` for the full id)

Wi-Fi fields and signal strength refresh once per second; the gateway state
updates from the node connect/disconnect events. `display.show` content from
the gateway fills the area above the footer, so connection status stays
visible at all times.

## Prepare The Gateway

If the board will connect over Wi-Fi to a gateway running on another machine, set `gateway.bind` to `lan` first. The default loopback bind is only reachable from the gateway host itself.

Allow this example's commands before pairing the board:

Warning: this command replaces the existing `gateway.nodes.allowCommands` value in the active profile.

```bash
openclaw config set gateway.bind lan
openclaw config set gateway.nodes.allowCommands '[
  "device.info",
  "device.status",
  "wifi.status",
  "display.show",
  "display.status"
]' --strict-json

openclaw gateway restart
openclaw gateway status --probe --json
```

These steps start from an existing OpenClaw gateway that the board can reach on your LAN.

## Build

```bash
. ~/esp-idf/export.sh
cd /path/to/repo/examples/esp-stickc
idf.py set-target esp32
idf.py build
```

## Flash

```bash
. ~/esp-idf/export.sh
cd /path/to/repo/examples/esp-stickc
idf.py -p <serial-port> flash monitor
```

## Main REPL Commands

After boot, the example starts the same serial REPL used by the generic ESP32
node example. On the M5StickC Plus2 the REPL is exposed over the USB-UART
bridge (CP210x or similar, depending on board revision).

The example automatically requests saved-session reconnect after Wi-Fi obtains
an IP and after ordinary connection-loss events. If no saved reconnect session
exists yet, those reconnect attempts are skipped and the board waits for an
explicit gateway auth command.

Start with these commands:

- `status` print saved-session availability and Wi-Fi state
- `wifi set <ssid> [passphrase]` store Wi-Fi credentials in NVS and connect immediately
  - Use `wifi set <ssid>` for an open network.
  - Use `wifi set <ssid> <passphrase>` for a secured network.

## First Connection

After Wi-Fi is up, pair the board with an OpenClaw gateway:

- `gateway setup-code <setup-code>` request one setup-code connect attempt; if Wi-Fi is still coming up, the REPL waits for an IP first
- `gateway token <uri> <token>` request one explicit shared-token connect attempt
- `gateway password <uri> <password>` request one explicit password connect attempt
- `gateway no-auth <uri>` request one explicit no-auth connect attempt
- `gateway connect` request one reconnect attempt using the saved reconnect session immediately

## Use The Node

Once the node is connected, the display updates in real time as the gateway sends `display.show` commands. The REPL stays available for debugging.

## Other CLI Commands

- `reboot` restart the device
- `wifi status` print current Wi-Fi state, IP, and RSSI
- `wifi clear` erase saved Wi-Fi credentials and disconnect
- `gateway status` print current OpenClaw connection state

## Troubleshooting And Reference

- **Blank screen on boot.** Confirm GPIO27 is HIGH (backlight) and that the ST7789 offsets x_gap=52, y_gap=40 are applied. This example handles both.
- **Device reboots on battery.** GPIO4 (power latch) must be HIGH. This example drives it HIGH at startup.
- **`idf.py set-target` must be `esp32`** not `esp32s3`. The M5StickC Plus2 uses the original ESP32-PICO.
- **LVGL font size.** The 135x240 screen is small. This example uses Montserrat 14 as the largest font to keep text readable without overflow.

## Notes

- The `esp32` target is fixed for this example.
- The node display name is `OpenClaw M5StickC Plus2`.
- This example uses the built-in `esp_lcd_new_panel_st7789()` from `esp_lcd_panel_vendor.h` (ESP-IDF >= 5.3) for direct panel control, not a board-level BSP.
- See [Component README](../../components/esp-openclaw-node/README.md) for the shared Node library.
