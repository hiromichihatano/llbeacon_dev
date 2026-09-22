# LLBeacon development instructions

## Build, flash, and test

This is a PlatformIO project targeting two M5Stack boards with the ESP-IDF
framework: AtomS3 Lite (`env:m5stack-atoms3`, ESP32-S3) and Atom Lite
(`env:m5stack-atom`, ESP32). Run commands from the repository root, replacing
`<env>` with the target environment:

```sh
# Build the firmware for both boards
pio run -e m5stack-atoms3 -e m5stack-atom

# Build/flash/monitor a single board
pio run -e <env>
pio run -e <env> -t upload
pio device monitor -e <env>

# Run all PlatformIO tests
pio test -e <env>

# Run one named test suite in test/<suite-name>/
pio test -e <env> -f <suite-name>
```

No lint or formatting command is configured. The native ESP-IDF CMake entry
point is also present: after exporting the ESP-IDF environment, use
`idf.py build`.

## Architecture

- `platformio.ini` defines one environment per supported board. Each
  environment sets a `-DLLBEACON_BOARD_*` build flag (`LLBEACON_BOARD_ATOMS3_LITE`
  or `LLBEACON_BOARD_ATOM_LITE`) that selects the board at compile time.
  Adding a new board (e.g. a future S3Matrix/S3R) means adding a new
  `[env:...]` section with its own `LLBEACON_BOARD_*` flag.
- `include/llbeacon_board.h` maps each `LLBEACON_BOARD_*` flag to that board's
  GPIO assignments (`LLBEACON_BUTTON_GPIO`, `LLBEACON_RGB_LED_GPIO`,
  `LLBEACON_RGB_LED_COUNT`, `LLBEACON_IR_GPIO`, `LLBEACON_I2C_SDA_GPIO`,
  `LLBEACON_I2C_SCL_GPIO`).
  Application code must use these symbolic names, never raw GPIO numbers, so
  the same source builds correctly for every board. The header fails the
  build (`#error`) if no known board flag is defined; extend it with an
  `#elif defined(...)` branch when adding a new board.
- The root `CMakeLists.txt` bootstraps ESP-IDF. `src/CMakeLists.txt` registers
  the application component and recursively adds every file under `src/` as a
  component source.
- ESP-IDF starts the firmware at `app_main()` in `src/main.c`; it is currently
  the only application source and blinks all onboard RGB LEDs red at 500 ms
  intervals. It uses Espressif's `espressif/led_strip` component with the RMT
  backend. Add firmware behavior from that ESP-IDF entry point and split
  reusable modules beneath `src/` as the application grows.
- Put application headers in `include/`, private PlatformIO libraries in
  `lib/<library>/`, and PlatformIO test suites in `test/<suite-name>/`.
  PlatformIO compiles private libraries and discovers their dependencies from
  source includes.
- `sdkconfig.defaults` disables peripherals not used by this project
  (`CONFIG_ETH_USE_SPI_ETHERNET`, `CONFIG_ETH_USE_ESP32_EMAC`,
  `CONFIG_BT_ENABLED`), keeping USB/UART console, GPIO, and LED support
  enabled. Wi-Fi hardware-capability flags (`CONFIG_SOC_WIFI_SUPPORTED`,
  `CONFIG_ESP_WIFI_ENABLED`) cannot be disabled via Kconfig and carry no
  runtime cost unless application code calls `esp_wifi_init()`. When enabling
  a new peripheral for a future feature (e.g. an I2S speaker HAT), add its
  Kconfig option here rather than relying on ESP-IDF defaults.
- External ESP-IDF dependencies are declared in `src/idf_component.yml`.
  `dependencies.lock.esp32` and `dependencies.lock.esp32s3` pin exact
  component versions for each target and must be committed. The downloaded
  `managed_components/` directory is generated and gitignored.

## Project-specific conventions

- Preserve the ESP-IDF application model: use `app_main()`, not a hosted C
  `main()` function.
- `sdkconfig.m5stack-atoms3` and `sdkconfig.m5stack-atom` are ESP-IDF-generated
  configurations for each board (ESP-IDF 6.1.0). Do not edit generated
  settings by hand; update them through `sdkconfig.defaults` or the
  ESP-IDF/PlatformIO configuration flow, then regenerate and retain the
  generated files.
- `.vscode/launch.json` and `.vscode/c_cpp_properties.json` are
  PlatformIO-generated, include machine-specific absolute paths, and are
  gitignored. Regenerate them through PlatformIO rather than manually
  adapting paths for a different workstation.
