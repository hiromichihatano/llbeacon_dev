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
コンポーネントを使用し、内蔵 RGB LED を赤色で 500 ms 間隔に点滅させる FreeRTOS
タスクを起動します（[`src/led_blink.cpp`](src/led_blink.cpp) /
[`include/led_blink.h`](include/led_blink.h)）。

また、コンソールと共用の UART0 上で [olmanqj/embedded-cli] ライブラリを使った
簡易 CLI を提供しています（[`src/uart_cli.cpp`](src/uart_cli.cpp) /
[`include/uart_cli.h`](include/uart_cli.h)）。UART 受信は ESP-IDF ドライバの
割り込み駆動イベントキューで処理し、専用 FreeRTOS タスクがキューから受信データを
取り出して CLI に渡します。以下のコマンドに対応しています。

- `led 0` — LED 点滅を無効化（消灯）
- `led 1` — LED 点滅を有効化

[olmanqj/embedded-cli]: https://registry.platformio.org/libraries/olmanqj/embedded-cli

アプリケーションコードは C++（ESP-IDF の `app_main()` は `extern "C"` で宣言）
で記述しています。関数には Doxygen 形式・日本語のコメントを付ける方針です。

未使用の Ethernet と Bluetooth は、フラッシュ容量とビルド時間を節約するため
[`sdkconfig.defaults`](sdkconfig.defaults) でデフォルト無効にしています。
USB/UART コンソール、GPIO、LED と、将来的な GPIO 接続のスピーカー HAT などに
必要となる機能は有効のままです。

## 依存ライブラリ・ファイルの置き場所

- `platformio.ini` の `lib_deps` で指定した PlatformIO ライブラリ（例:
  `olmanqj/embedded-cli`）は、ビルド後に環境ごとの
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
[`.github/copilot-instructions.md`](.github/copilot-instructions.md) を参照してください。
