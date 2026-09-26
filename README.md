# llbeacon_dev
LLBeacon デバイス

## 概要

単一のソースツリーから、以下 2 種類の M5Stack ボード向けファームウェアを
ビルドできる PlatformIO + ESP-IDF プロジェクトです。

- **AtomS3 Lite** (ESP32-S3) — `pio run -e m5stack-atoms3`
- **Atom Lite** (ESP32) — `pio run -e m5stack-atom`

ボード固有の GPIO 割り当て（ボタン、RGB LED、IR、I2C）は
[`include/llbeacon_board.h`](include/llbeacon_board.h) に集約しています。
[`platformio.ini`](platformio.ini) の各環境で設定される `LLBEACON_BOARD_*`
フラグにより、コンパイル時に対象ボードを選択します。アプリケーションコードでは
GPIO 番号を直接使用せず、このヘッダのシンボル名を使用してください。新しいボードを
追加しても、共通のアプリケーションコードをそのまま利用できます。

現在のファームウェアは、Espressif 公式の
[`espressif/led_strip`](https://components.espressif.com/components/espressif/led_strip)
コンポーネントを使用し、内蔵 RGB LED をパターン（PULSE / FLASH / SAW / SINE）で
点灯させます。10 ms 周期の FreeRTOS タスクがパターン描画と dimmer モード遷移を
行います（[`src/led_control.cpp`](src/led_control.cpp) /
[`include/led_control.h`](include/led_control.h)）。ボタンの短押 / 長押は
[`src/button.cpp`](src/button.cpp) / [`include/button.h`](include/button.h) で
判定し、dimmer モード遷移に反映します。

また、[funbiscuit/embedded-cli] ライブラリを使った簡易 CLI を提供しています
（[`src/uart_cli.cpp`](src/uart_cli.cpp) /
[`include/uart_cli.h`](include/uart_cli.h)）。入出力先はボードで切り替わり、
Atom Lite は UART0、AtomS3 Lite は USB Serial JTAG を使用します。専用
FreeRTOS タスクが受信データを取り出して CLI に渡します。以下のコマンドに
対応しています。

- `led set <pulse|flash|saw|sine> <RRGGBB> <RRGGBB> <100-60000>` — 光らせ方を一括設定
- `led max <0-255>` — 最大輝度を設定
- `led dim <active|dimmer1|dimmer2> <0-100>` — dimmer 輝度%を設定
- `led time <dimmer1|dimmer2|notification> <1-86400>` — dimmer 時間を設定
- `led mode <active|dimmer1|dimmer2|sleep|notification>` — 強制モード遷移
- `led status [--json]` — 現在の設定を表示
- `tone <sine|square|saw> <freq:duration[:volume]> ...` — シーケンス再生
- `sound volume master <0-100>` — master volume を設定
- `sound status [--json]` — 現在の状態を表示
- `sound stop` — 再生を停止

`tone` の note は `周波数:ミリ秒[:音量]` をスペース区切りで並べます。
周波数 `0` は無音、音量を省略すると `80` になります。

```text
tone sine 2000:60 1000:80
tone square 1500:50 0:30 1500:50
```

AtomS3 Lite + Atomic Voice Base (A149) では、`tone` コマンドで sine /
square / saw のトーンを再生できます（[`src/audio_tone.cpp`](src/audio_tone.cpp) /
[`include/audio_tone.h`](include/audio_tone.h)、[esp_codec_dev] 使用）。
最終音量は master volume と note 個別音量の積です。

仕様の詳細は [`docs/led-control-requirements.md`](docs/led-control-requirements.md)、
設計は [`docs/led-control-design.md`](docs/led-control-design.md) と
[`docs/led-control-ui.md`](docs/led-control-ui.md) を参照してください。

[funbiscuit/embedded-cli]: https://github.com/funbiscuit/embedded-cli
[esp_codec_dev]: https://components.espressif.com/components/espressif/esp_codec_dev

アプリケーションコードは C++（ESP-IDF の `app_main()` は `extern "C"` で宣言）
で記述しています。関数には Doxygen 形式・日本語のコメントを付ける方針です。

未使用の Ethernet と Bluetooth は、フラッシュ容量とビルド時間を節約するため
[`sdkconfig.defaults`](sdkconfig.defaults) でデフォルト無効にしています。
USB/UART コンソール、GPIO、LED、I2S オーディオ（Atomic Voice Base）に
必要となる機能は有効のままです。

## 依存ライブラリ・ファイルの置き場所

- `platformio.ini` の `lib_deps` で指定したライブラリ（例:
  `https://github.com/funbiscuit/embedded-cli.git#v0.1.4`）は、ビルド後に環境ごとの
  `.pio/libdeps/<env>/<ライブラリ名>/` 以下にダウンロードされます
  （生成物のため Git 管理外・未ビルド時は存在しません）。ヘッダは
  そのライブラリの `lib/include/` 配下にあります。
- ESP-IDF フレームワーク本体（`driver/uart.h` や FreeRTOS ヘッダなど）は
  `~/.platformio/packages/framework-espidf/components/` 以下にあります。
- クロスコンパイラ（`xtensa-esp32s3-elf-gcc` 等）と、それに付属する
  libc/libstdc++ ヘッダは `~/.platformio/packages/toolchain-xtensa-esp-elf/`
  以下にあります。
- ESP-IDF の Managed Components（`src/idf_component.yml` で宣言）は
  `managed_components/` 以下に展開されます（生成物のため Git 管理外）。

これらの場所が分かっているため、ファイル探索はリポジトリ直下または
ホームディレクトリ以下に限定してください（ファイルシステム全体を検索する
必要はありません）。

ビルド・書き込み・テストの詳細なコマンドとプロジェクトの規約は
[`AGENTS.md`](AGENTS.md) にまとめています。GitHub Copilot のうち `AGENTS.md` を
読まない画面（github.com の Copilot Chat、Visual Studio / JetBrains / Eclipse /
Xcode のチャットなど）向けの要約は
[`.github/copilot-instructions.md`](.github/copilot-instructions.md) にあります。
