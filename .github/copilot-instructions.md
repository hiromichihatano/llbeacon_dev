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
- ESP-IDF starts the firmware at `app_main()` in `src/main.cpp`. Application
  sources are C++ (ESP-IDF's `app_main()` must stay declared `extern "C"`
  since ESP-IDF calls it with C linkage). LED blinking lives in
  `src/led_blink.cpp` / `include/led_blink.h` (starts a FreeRTOS task,
  exposes `led_blink_start()` / `led_blink_set_enabled()`); the UART CLI
  lives in `src/uart_cli.cpp` / `include/uart_cli.h` (reads UART0 via the
  ESP-IDF driver's interrupt-driven event queue and feeds bytes to an
  `embedded-cli` instance in a dedicated FreeRTOS task; supports `led 0` /
  `led 1` commands). Split further reusable modules beneath `src/` (with a
  matching header in `include/`) as the application grows.
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
- PlatformIO library dependencies (e.g. `olmanqj/embedded-cli`, declared as
  `lib_deps` in `platformio.ini`) are downloaded per-environment under
  `.pio/libdeps/<env>/<library-name>/` (generated, gitignored, not present
  until after a build). Their headers (e.g. `embedded_cli.h`) live under
  that library's `lib/include/` subdirectory. Do not search the whole
  filesystem for these; check `.pio/libdeps/<env>/` first, and fall back to
  `~/.platformio/packages/framework-espidf/components/` for ESP-IDF
  framework components (e.g. `driver/uart.h`, FreeRTOS headers) or
  `~/.platformio/packages/toolchain-xtensa-esp-elf/` for the cross-compiler
  toolchain and its bundled libc/libstdc++ headers. All file/text searches
  should stay scoped under the repository root or `$HOME`, never `/`.

## Project-specific conventions

- Preserve the ESP-IDF application model: use `app_main()`, not a hosted C
  `main()` function.
- Application code is C++. Add a Doxygen-style comment (in Japanese) to every
  function; on public API declared in `include/*.h`, put the full comment on
  the header declaration and use `/** @copydoc <name> */` on the matching
  definition in `src/*.cpp` to avoid duplicating it.
- Initialize ESP-IDF/driver structs (e.g. `uart_config_t`, `CliCommandBinding`,
  `led_strip_config_t`) with a single C++20 designated-initializer literal
  (`const T value = { .field = ..., .nested = {.field = ...} };`) instead of
  declaring `T value = {};` and assigning each field afterward. Prefer `const`
  for structs that are not mutated after initialization. See
  `src/led_blink.cpp` and `src/uart_cli.cpp` for examples.
- `sdkconfig.m5stack-atoms3` and `sdkconfig.m5stack-atom` are ESP-IDF-generated
  configurations for each board (ESP-IDF 6.1.0). Do not edit generated
  settings by hand; update them through `sdkconfig.defaults` or the
  ESP-IDF/PlatformIO configuration flow, then regenerate and retain the
  generated files.
- `.vscode/launch.json` and `.vscode/c_cpp_properties.json` are
  PlatformIO-generated, include machine-specific absolute paths, and are
  gitignored. Regenerate them through PlatformIO rather than manually
  adapting paths for a different workstation.
