# LLBeacon development instructions

## Build, flash, and test

This is a PlatformIO project targeting an M5Stack AtomS3 (`esp32s3`) with the
ESP-IDF framework. Run commands from the repository root:

```sh
# Build the firmware
pio run -e m5stack-atoms3

# Flash the connected board
pio run -e m5stack-atoms3 -t upload

# Open the serial monitor
pio device monitor -e m5stack-atoms3

# Run all PlatformIO tests
pio test -e m5stack-atoms3

# Run one named test suite in test/<suite-name>/
pio test -e m5stack-atoms3 -f <suite-name>
```

No lint or formatting command is configured. The native ESP-IDF CMake entry
point is also present: after exporting the ESP-IDF environment, use
`idf.py build`.

## Architecture

- `platformio.ini` defines the single build environment. Keep the environment
  name `m5stack-atoms3` in commands and generated debug configuration.
- The root `CMakeLists.txt` bootstraps ESP-IDF. `src/CMakeLists.txt` registers
  the application component and recursively adds every file under `src/` as a
  component source.
- ESP-IDF starts the firmware at `app_main()` in `src/main.c`; it is currently
  the only application source and intentionally contains an empty entry point.
  Add firmware behavior from that ESP-IDF entry point and split reusable
  modules beneath `src/` as the application grows.
- Put application headers in `include/`, private PlatformIO libraries in
  `lib/<library>/`, and PlatformIO test suites in `test/<suite-name>/`.
  PlatformIO compiles private libraries and discovers their dependencies from
  source includes.

## Project-specific conventions

- Preserve the ESP-IDF application model: use `app_main()`, not a hosted C
  `main()` function.
- `sdkconfig.m5stack-atoms3` is an ESP-IDF-generated configuration for this
  board (ESP-IDF 6.1.0, 2 MB flash, single-app partition layout). Do not edit
  generated settings by hand; update them through the ESP-IDF/PlatformIO
  configuration flow and retain the generated file.
- `.vscode/launch.json` is PlatformIO-generated and includes
  machine-specific absolute paths. Regenerate it through PlatformIO rather
  than manually adapting paths for a different workstation.
