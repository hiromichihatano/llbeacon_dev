# LLBeacon — project instructions

PlatformIO + ESP-IDF firmware for the LLBeacon device: one source tree, two
M5Stack targets (AtomS3 Lite / ESP32-S3, Atom Lite / ESP32). This file uses the
cross-tool `AGENTS.md` name, so Reasonix, Codex, Cursor, and other AGENTS.md-aware
agents all read it as standing instructions. `README.md` is the Japanese
user-facing overview; `.github/copilot-instructions.md` stays thin — it points
back here for the Copilot surfaces that cannot read this file. Keep this file,
that file, and the README consistent when commands or conventions change, and put
shared contracts here rather than duplicating them there.

## Build / flash / test

Run from the repository root. PlatformIO Core (`pio`) resolves on `PATH`;
`<env>` is `m5stack-atoms3` or `m5stack-atom`.

```sh
pio run -e m5stack-atoms3 -e m5stack-atom   # build both boards
pio run -e <env>                            # build one board
pio run -e <env> -t upload                  # flash
pio device monitor -e <env>                 # serial monitor
pio test -e <env> -f <suite>                # one suite from test/<suite>/
```

No lint or format command is configured. `test/` currently holds only
PlatformIO's placeholder README, so no test suite exists yet. The native ESP-IDF
entry point also works: export the ESP-IDF environment, then `idf.py build`.

Both boards must build for a change to be complete — that is the only automated
check here (no on-device CI; verify hardware behaviour by flashing and watching
`pio device monitor -e <env>`).

## GitHub issues

The repository is `hiromichihatano/llbeacon_dev` on GitHub (`origin` points at
it), and the `gh` CLI is installed and already authenticated as
`hiromichihatano` (verify with `gh auth status`). Read and update issues through
`gh` rather than by fetching the web UI:

```sh
gh issue list --repo hiromichihatano/llbeacon_dev
gh issue view <number> --repo hiromichihatano/llbeacon_dev
gh issue comment <number> --repo hiromichihatano/llbeacon_dev --body '<text>'
gh issue create --repo hiromichihatano/llbeacon_dev \
  --title '<title>' --body '<text>'
```

Issue text is in Japanese. Prefer `gh` over `curl`/web search for anything in
this repository, and cite the issue number when a change implements one.

## Board abstraction

- `platformio.ini` defines one `[env:...]` per board, each setting exactly one
  `-DLLBEACON_BOARD_*` flag (`LLBEACON_BOARD_ATOMS3_LITE`,
  `LLBEACON_BOARD_ATOM_LITE`) that selects the board at compile time.
- `include/llbeacon_board.h` maps each flag to that board's GPIO names:
  `LLBEACON_BUTTON_GPIO`, `LLBEACON_RGB_LED_GPIO`, `LLBEACON_RGB_LED_COUNT`,
  `LLBEACON_IR_GPIO`, `LLBEACON_I2C_SDA_GPIO`, `LLBEACON_I2C_SCL_GPIO`.
  Application code must use these symbols, never raw GPIO numbers, so one
  source builds for every board. The header `#error`s when no known board flag
  is defined.
- Adding a board means a new `[env:...]` section with its own flag plus an
  `#elif defined(...)` branch in that header; shared application code stays
  unchanged.

## Layout and build wiring

- `src/` application sources (C++), public headers in `include/`, private
  PlatformIO libraries in `lib/<library>/`, test suites in `test/<suite-name>/`.
  `src/CMakeLists.txt` globs every file under `src/`, so new `.cpp` files are
  picked up without editing it.
- ESP-IDF starts the firmware at `app_main()` in `src/main.cpp`. Keep the
  ESP-IDF application model (not a hosted `main()`) and keep it declared
  `extern "C"`, since ESP-IDF calls it with C linkage.
- Existing modules, all wrapped in the `llbeacon` namespace — follow that for
  new modules:
  - `src/led_control.cpp` / `include/led_control.h` — owns the `led_strip`
    handle and a 10 ms FreeRTOS task that renders the current pattern and
    applies dimmer mode transitions.
  - `src/button.cpp` / `include/button.h` — GPIO ISR plus a FreeRTOS task that
    classifies short/long presses and forwards them to `led_control`.
  - `src/uart_cli.cpp` / `include/uart_cli.h` — feeds an `embedded-cli` instance
    in a dedicated FreeRTOS task. Input/output transport is board-selected:
    UART0 on Atom Lite, USB Serial JTAG on AtomS3 Lite. Supports the
    `led set|max|dim|time|mode|status` subcommands.
- `sdkconfig.defaults` disables peripherals the project does not use
  (`CONFIG_ETH_USE_SPI_ETHERNET`, `CONFIG_ETH_USE_ESP32_EMAC`, `CONFIG_BT_ENABLED`)
  while keeping the USB/UART console, GPIO, and LED support. Enable a new
  peripheral here (e.g. I2S for a future speaker HAT), not in the generated
  files. `CONFIG_SOC_WIFI_SUPPORTED` / `CONFIG_ESP_WIFI_ENABLED` cannot be
  disabled via Kconfig and cost nothing unless application code calls
  `esp_wifi_init()`.
- `sdkconfig.m5stack-atoms3` and `sdkconfig.m5stack-atom` are ESP-IDF 6.1.0
  generated files marked "DO NOT EDIT". Change `sdkconfig.defaults` or the
  PlatformIO/ESP-IDF configuration flow, then regenerate and keep the results.
- `src/idf_component.yml` declares ESP-IDF components (currently
  `espressif/led_strip`), which are downloaded into the gitignored
  `managed_components/`. `dependencies.lock.esp32` / `dependencies.lock.esp32s3`
  pin exact versions per target and must be committed.
- PlatformIO library dependencies (`lib_deps` in `platformio.ini`, e.g.
  `olmanqj/embedded-cli`) are downloaded per environment into
  `.pio/libdeps/<env>/<library>/` — generated, gitignored, and absent until a
  build runs.
- `.vscode/launch.json` and `.vscode/c_cpp_properties.json` are
  PlatformIO-generated, contain machine-specific absolute paths, and are
  gitignored. Regenerate them through PlatformIO instead of adapting paths by
  hand.

## Code conventions

- Application code is C++20. Give every function a Doxygen-style comment in
  Japanese: put the full comment on the `include/*.h` declaration and
  `/** @copydoc <name> */` on the matching `src/*.cpp` definition, so the text is
  not duplicated.
- Initialize ESP-IDF/driver structs (`uart_config_t`, `CliCommandBinding`,
  `led_strip_config_t`, ...) with a single C++20 designated-initializer literal
  (`const T value = { .field = ..., .nested = {.field = ...} };`) instead of
  declaring `T value = {};` and assigning fields afterwards. Prefer `const` for
  structs that are not mutated after initialization. See `src/led_control.cpp`,
  `src/button.cpp`, and `src/uart_cli.cpp`.

## Finding third-party headers

Do not scan the filesystem. Check, in this order:
`.pio/libdeps/<env>/<library>/lib/include/` (PlatformIO libraries, e.g.
`embedded_cli.h`), `~/.platformio/packages/framework-espidf/components/` (ESP-IDF
framework components such as `driver/uart.h` and the FreeRTOS headers), then
`~/.platformio/packages/toolchain-xtensa-esp-elf/` (cross-compiler toolchain and
its bundled libc/libstdc++ headers). Keep all file and text searches under the
repository root or `$HOME`, never `/`.
