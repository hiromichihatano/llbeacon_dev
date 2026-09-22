# llbeacon_dev
LLBeacon Device

## Overview

PlatformIO + ESP-IDF firmware project supporting two M5Stack boards from a
single source tree:

- **AtomS3 Lite** (ESP32-S3) — `pio run -e m5stack-atoms3`
- **Atom Lite** (ESP32) — `pio run -e m5stack-atom`

Board-specific GPIO assignments (button, RGB LED, IR, I2C) are centralized in
[`include/llbeacon_board.h`](include/llbeacon_board.h) and selected at compile
time via the `LLBEACON_BOARD_*` flag set per environment in
[`platformio.ini`](platformio.ini). Application code should always use the
symbolic GPIO names from that header instead of raw pin numbers, so it keeps
working when a new board target is added.

The current firmware blinks the onboard RGB LED red at 500 ms intervals using
Espressif's official [`espressif/led_strip`](https://components.espressif.com/components/espressif/led_strip)
component.

Unused peripherals (Ethernet, Bluetooth) are disabled by default in
[`sdkconfig.defaults`](sdkconfig.defaults) to save flash and build time; the
USB/UART console, GPIO, and LED support required for current and planned
hardware (e.g. a future GPIO-connected speaker HAT) remain enabled.

See [`.github/copilot-instructions.md`](.github/copilot-instructions.md) for
detailed build/flash/test commands and project conventions.
