# Testing Cardputer-Adv firmware without the device

This note compares the ways to exercise this firmware without a Cardputer-Adv on the desk, and
documents the one that works today: the native Linux build (`meshtasticd`) with BaseUI drawn into a
240x135 SDL window.

## Options at a glance

| Option                                         | What it tests                                                               | Display/keyboard                                     | Effort                     | Verdict                         |
| ---------------------------------------------- | --------------------------------------------------------------------------- | ---------------------------------------------------- | -------------------------- | ------------------------------- |
| **Native `native-tft` + SDL window** (this PR) | BaseUI screens, menus, mesh logic, sim radio, API (CLI, web client, MCP)    | 240x135 window, arrow keys + Enter                   | Works now                  | **Use this for UI work**        |
| Native unit tests (`bin/run-tests.sh`)         | Pure logic (tile math, settings parsing, routing, NodeDB)                   | None                                                 | Works now                  | Use for logic you can isolate   |
| Wokwi (ESP32-S3)                               | The real ESP32 binary, boot, some peripherals                               | ILI9341-class SPI displays; no TCA8418 keyboard, no LoRa cap, no Cardputer board | Custom chips + paid CLI for CI | Not worth it for this device    |
| Espressif QEMU (ESP32-S3)                      | The real ESP32 binary, boot, crashes, flash/partition layout                | Only the virtual RGB panel (`esp_lcd_qemu_rgb`), not the ST7789 over SPI; no keyboard | Boot/crash checks only | Useful for "does it boot", not UI |

Native is the practical choice because the UI code (`src/graphics/**`) is shared between ESP32 and
Linux, so the same BaseUI frames, menus and banners render pixel-for-pixel in the window. What it does
not cover is ESP32-specific hardware (ST7789 driver, I2S audio, TCA8418 keyboard, SD over SPI, PSRAM
and heap limits) - those still need the real device.

## Quick start (Linux)

```bash
# Arch / Omarchy
sudo pacman -S --needed sdl2 yaml-cpp libgpiod libusb libuv libx11 libinput libxkbcommon openssl \
  bluez-libs jsoncpp curl sqlite i2c-tools
# (libulfius is optional; it only enables the built-in web server)

# Debian / Ubuntu
sudo apt install libsdl2-dev libyaml-cpp-dev libgpiod-dev libusb-1.0-0-dev libuv1-dev libx11-dev \
  libinput-dev libxkbcommon-x11-dev libssl-dev libbluetooth-dev libjsoncpp-dev libcurl4-openssl-dev \
  libsqlite3-dev libi2c-dev

bin/cardputer-sim.sh            # builds native-tft once, then opens the window
bin/cardputer-sim.sh --fresh    # start as a brand-new node (wipes ~/.portduino/cardputer)
```

Keys in the window: arrows navigate, Enter selects. Closing the window stops the node. The node
listens on TCP 4403, so `meshtastic --host localhost`, the web client and meshtastic-mcp
(`MESHTASTIC_MCP_TCP_HOST=localhost`) can talk to it. Run a second node with
`SIM_NAME=b SIM_MAC=AA:BB:CC:DD:EE:02 SIM_PORT=4404 bin/cardputer-sim.sh` to see two sims mesh over
the simulated radio.

On Wayland (Omarchy/Hyprland) SDL uses XWayland or native Wayland automatically; if the window does
not appear, try `SDL_VIDEODRIVER=x11`.

### Headless screenshots (CI, agents)

```bash
bin/cardputer-sim.sh --no-build --fresh --screenshot /tmp/ui.png --wait 15
```

Runs under `xvfb-run`, waits, and saves the window at 3x scale (needs `xvfb`, ImageMagick and
`xwininfo` from x11-utils / xorg-xwininfo). Send keys before the shot with `xdotool keydown Right;
xdotool keyup Right` if you drive Xvfb yourself; SDL ignores synthetic `xdotool key --window` events.

### macOS

The `native-macos` env builds without a screen (`HAS_SCREEN=0`), so there is no window on a Mac. Use
the Linux machine, or `bin/test-native-docker.sh` for unit tests.

## Config used

[`bin/cardputer-sim/config.yaml`](../bin/cardputer-sim/config.yaml): sim radio, X11 panel at
240x135 with `OffsetRotate: 0`, and `DisplayMode: TWOCOLOR` to force BaseUI instead of MUI.

`TFTDisplay` used to give BaseUI a square canvas on every Linux TFT panel (height x height without
`Rotate`), which squeezed a 240x135 window into a 135x135 layout. For the X11 panel only it now uses
the real logical size; SPI panels are unchanged.

## Not covered yet: Cardputer-only screens

Everything behind `#ifdef M5STACK_CARDPUTER_ADV` (the offline GPS map, Cardputer menus, SD-backed
settings, colour regions tuned for the device) is not compiled into `native-tft`. Bringing the map
into the simulator needs:

1. a `native-cardputer` env that defines a UI-only flag (for example `CARDPUTER_UI`) and
   `-D VIEW_240x135`-style sizing, separate from `M5STACK_CARDPUTER_ADV`, which also pulls in ESP32
   audio, keyboard and SD code;
2. `OfflineMapRenderer` reading tiles through a small file shim, using `SD` on the device and the
   portduino filesystem (`~/.portduino/<name>/maps/...`) on Linux;
3. JPEGDEC added to that env's `lib_deps` (it is portable C);
4. a fixed GPS position for the simulated node, set via `meshtastic --host localhost --setlat ... --setlon ...`.
