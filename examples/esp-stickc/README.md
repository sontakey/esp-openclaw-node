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

Capabilities: `device`, `wifi`, `display`, `imu`, `buzzer`, `led`, `battery`,
`button`, `motion`

Commands:

- `device.info`
- `device.status`
- `wifi.status`
- `display.show`
- `display.status`
- `display.menu`
- `imu.read`
- `buzzer.beep`
- `led.set`
- `battery.status`
- `button.status`
- `motion.status`

## Hardware Commands

Beyond the display, the example exposes the M5StickC Plus2's on-board
peripherals as OpenClaw commands. Invoke them with
`openclaw nodes invoke --node <id> --command <name> --params <json>`.

| Command | Params | Returns |
|---------|--------|---------|
| `imu.read` | none | MPU6886 `accel` (g), `gyro` (dps), `tempC` (IMU die temperature) |
| `buzzer.beep` | `frequency` Hz (100-10000, default 4000), `durationMs` (1-1000, default 200) | the clamped `frequency` and `durationMs` actually used |
| `led.set` | `on` (boolean, required) | the resulting `on` state |
| `battery.status` | none | `millivolts`, `volts`, `percent` (rough 3.0-4.2 V curve), `raw` ADC |
| `button.status` | none | per button `a`/`b`: `pressed` plus `pressCount` (cumulative since boot - diff it between polls) |
| `motion.status` | none | `orientation`, `pitchDeg`, `rollDeg`, `moving` - derived from the IMU |

Examples:

```bash
openclaw nodes invoke --node <id> --command imu.read --json
openclaw nodes invoke --node <id> --command buzzer.beep --params '{"frequency":3000,"durationMs":300}' --json
openclaw nodes invoke --node <id> --command led.set --params '{"on":true}' --json
openclaw nodes invoke --node <id> --command battery.status --json
openclaw nodes invoke --node <id> --command button.status --json
openclaw nodes invoke --node <id> --command motion.status --json
```

Peripheral bring-up is best-effort: if the IMU or battery ADC fails to
initialize, its command is still registered but returns an `UNAVAILABLE`
error instead of crashing the node.

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

## Interactive Menu

`display.menu` turns the StickC into a physical confirm/choose device. It
shows a title and a list of options, then **blocks** until the user picks one
with the buttons — **A** moves the highlight, **B** confirms — or the timeout
elapses.

```bash
openclaw nodes invoke --node <id> --command display.menu \
  --params '{"title":"Deploy?","options":["Yes","No"],"timeoutMs":30000}' \
  --invoke-timeout 35000 --json
# -> {"timedOut":false,"selected":0,"label":"Yes"}   (or {"timedOut":true})
```

`options` is a 1-8 entry string array; `timeoutMs` is clamped to 1-120 s
(default 30 s). Because the node only replies once the user answers, set the
CLI `--invoke-timeout` higher than `timeoutMs`.

## Connection Warning Footer

The small screen is kept clear whenever things are working: when Wi-Fi **and**
the gateway are both connected, nothing extra is shown and the whole screen is
available for content.

A single warning line appears at the bottom only when a link is down:

- **Wi-Fi not connected** the station has no connection. The gateway is
  unreachable in this state, so only this line is shown.
- **Gateway not connected** Wi-Fi is up but the OpenClaw session is not.

The footer is re-evaluated once per second from the live Wi-Fi state and the
node connect/disconnect events. `display.show` content from the gateway fills
the area above it.

## Power Saving

For battery use the LCD backlight - the dominant power draw - is turned off
after 30 seconds with no new content (`STICKC_BACKLIGHT_IDLE_TIMEOUT_MS`). The
node stays connected; the next `display.show` from the gateway lights the
screen again. Wi-Fi modem sleep (`WIFI_PS_MIN_MODEM`) is on by ESP-IDF default.

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
  "display.status",
  "imu.read",
  "buzzer.beep",
  "led.set",
  "battery.status"
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
