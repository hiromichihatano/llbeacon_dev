# LLBeacon — GitHub Copilot instructions

[`AGENTS.md`](../AGENTS.md) at the repository root is the authoritative source for
this project's build/flash/test commands, board abstraction, layout, and code
conventions. Read it before changing anything. This file is deliberately thin —
update `AGENTS.md` rather than duplicating its content here.

## Why this file exists

Copilot Chat on github.com and the Visual Studio / JetBrains / Eclipse / Xcode
chats read **only** this file. The cloud agent, code review, the CLI, and VS Code
also read `AGENTS.md`. Copilot code review reads roughly the first 4,000
characters of an instruction file, so keep this short, and add path-scoped rules
as `.github/instructions/<name>.instructions.md` with `applyTo:` globs instead of
growing this file.

## Non-obvious rules that must survive on every surface

- Build both boards from the repository root before calling a change complete:
  `pio run -e m5stack-atoms3 -e m5stack-atom`.
- Never hardcode GPIO numbers; use the `LLBEACON_*` symbols from
  `include/llbeacon_board.h`.
- Keep `app_main()` in `src/main.cpp` declared `extern "C"`.
- `sdkconfig.m5stack-atoms3` / `sdkconfig.m5stack-atom` are generated and marked
  "DO NOT EDIT"; change `sdkconfig.defaults` and regenerate instead.
- Application code is C++20; initialize ESP-IDF/driver structs with C++20
  designated initializers.
