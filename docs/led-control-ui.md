# LED 制御 UI 設計（button / uart_cli）

本ドキュメントは、LED 制御に対するユーザー入力（ボタン）とユーザー出力（UART CLI）の
設計をまとめたものです。対象要件は [led-control-requirements.md](led-control-requirements.md)、
LED 制御本体の設計は [led-control-design.md](led-control-design.md) を参照してください。

## 1. `button` モジュール

- `LLBEACON_BUTTON_GPIO` を `GPIO_MODE_INPUT` + 内部プルアップで初期化します。
  - M5Stack のボタンは押下で LOW になる想定です（実機で極性を確認します）。
- `gpio_install_isr_service()` + `gpio_isr_handler_add()` で両エッジ割り込みを
  設定し、ISR はエッジ種別と tick を queue に送るだけにします。

```text
  button GPIO
      |
      v
  ISR (both edges)
      |  queue send
      v
  button task
      |
      |  FALL: press start
      |        - LONG fired when held 1000ms or more
      |  RISE: held < 50ms      -> ignore (chatter)
      |         held 50-1000ms  -> SHORT
      v
  led_control::handle_button()
```

- LONG は押下継続中に即時発火します（離すのを待たない）。
- タスクは queue 受信をタイムアウト付きで待ち、LONG 判定を行います。

## 2. `uart_cli` モジュール

入出力トランスポートはボード定義で切り替え、受信バイトを embedded-cli へ渡す
FreeRTOS タスクを共通化します。

- Atom Lite (ESP32): UART0 の割り込み駆動イベントキュー
- AtomS3 Lite (ESP32-S3): USB Serial JTAG のポーリング読み出し

`led` binding は以下のサブコマンドに置き換えます。

### 2.1 コマンド解析

`embeddedCliGetToken(args, n)` でトークンを取得します。

| トークン位置 | `led set` | `led max` | `led dim` | `led time` | `led mode` | `led status` |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | `set` | `max` | `dim` | `time` | `mode` | `status` |
| 2 | pattern | 0-255 | target | target | mode | `--json`（任意） |
| 3 | rgb1 | - | 0-100 | 1-86400 | - | - |
| 4 | rgb2 | - | - | - | - | - |
| 5 | period_ms | - | - | - | - | - |

- hex は `RRGGBB` 固定（`#` なし）、大文字・小文字は許容します。
- パース失敗時は `ERR: ...` を表示し、設定を変更しません。

### 2.2 出力

- コマンド応答は `embeddedCliPrint()` で出力します（プロンプト再表示を壊さない）。
- `led status` は human-readable な複数行、`led status --json` は JSON 1 行です。
- JSON は固定バッファ（例: 512 バイト）に `snprintf()` で構築します。

### 2.3 `led status --json` のスキーマ

```json
{
  "mode": "active",
  "max_brightness": 128,
  "dimmer": {"active": 100, "dimmer1": 30, "dimmer2": 10, "sleep": 0},
  "time": {"dimmer1_s": 10, "dimmer2_s": 180, "notification_s": 5},
  "pattern": "PULSE",
  "rgb1": "FF0000",
  "rgb2": "000000",
  "period_ms": 1000
}
```
