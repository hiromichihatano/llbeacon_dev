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
コンポーネントを使用し、内蔵 RGB LED を赤色で 500 ms 間隔に点滅させます。

未使用の Ethernet と Bluetooth は、フラッシュ容量とビルド時間を節約するため
[`sdkconfig.defaults`](sdkconfig.defaults) でデフォルト無効にしています。
USB/UART コンソール、GPIO、LED と、将来的な GPIO 接続のスピーカー HAT などに
必要となる機能は有効のままです。

ビルド・書き込み・テストの詳細なコマンドとプロジェクトの規約は
[`.github/copilot-instructions.md`](.github/copilot-instructions.md) を参照してください。
