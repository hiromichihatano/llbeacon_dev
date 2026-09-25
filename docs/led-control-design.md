# LED 制御 設計

対象要件は [led-control-requirements.md](led-control-requirements.md) です。
本ドキュメントは、その要件を実装するための設計をまとめたものです。
UI（button / uart_cli）の詳細設計は [led-control-ui.md](led-control-ui.md) を参照してください。

## 1. 設計方針

- 既存の `led_blink`（500ms 固定点滅）と `led 0` / `led 1` コマンドを廃止し、
  LED 制御を新モジュール `led_control` に一本化する。
- ボタン入力は新モジュール `button` で扱う（現状は未実装のため追加する）。
- UART CLI は既存の UART ドライバ / embedded-cli の枠組みを流用し、
  コマンド定義のみを置き換える。
- 設定値の永続化はしない（NVS は使用しない）。

## 2. モジュール構成

```text
                     +---------------------------+
                     |         app_main          |
                     +-----+--------------+------+
                           |              |
            +--------------v--+        +--v---------------+
            |  uart_cli タスク |        |  button タスク    |
            |  (embedded-cli)  |        | (GPIO ISR + 判定) |
            +--------------+---+        +--+---------------+
                           |               |
              setter / getter              | handle_button()
                           |               |
                     +-----v---------------v------+
                     |         led_control         |
                     | 設定保持 / LEDタスク / モード遷移 |
                     +--------------+--------------+
                                    |
                                    v
                            led_strip (RMT)
```

- `uart_cli` と `button` は直接 `led_strip` に触れない。LED 出力は
  `led_control` の LED タスクだけが行う。
- `button` は判定結果を `led_control::handle_button()` に通知する。

## 3. ファイル変更一覧

| ファイル | 変更 |
| --- | --- |
| `include/led_control.h` / `src/led_control.cpp` | 新規追加 |
| `include/button.h` / `src/button.cpp` | 新規追加 |
| `include/uart_cli.h` / `src/uart_cli.cpp` | コマンド定義を全面改修 |
| `src/main.cpp` | 起動処理を差し替え |
| `include/led_blink.h` / `src/led_blink.cpp` | 削除 |
| `README.md` | コマンド表と現状説明を更新 |
| `AGENTS.md` | モジュール一覧・コマンド規約を更新 |

`src/CMakeLists.txt` は `src/` 配下を glob しているため変更不要です。
`platformio.ini` / `sdkconfig.defaults` / `src/idf_component.yml` も変更不要です。

## 4. `led_control` モジュール

### 4.1 型と状態

```cpp
namespace llbeacon::led_control {

enum class Mode { ACTIVE, DIMMER1, DIMMER2, SLEEP, NOTIFICATION };
enum class Pattern { PULSE, FLASH, SAW, SINE };
enum class DimmerTarget { ACTIVE, DIMMER1, DIMMER2 };  // sleep は 0% 固定
enum class TimeTarget { DIMMER1, DIMMER2, NOTIFICATION };

struct Status {
    Mode mode;
    uint8_t max_brightness;       // 0-255, default 128
    uint8_t dimmer_active;        // 0-100, default 100
    uint8_t dimmer_dimmer1;       // 0-100, default 80
    uint8_t dimmer_dimmer2;       // 0-100, default 30
    uint32_t time_dimmer1_s;      // default 10
    uint32_t time_dimmer2_s;      // default 180
    uint32_t time_notification_s; // default 5
    Pattern pattern;              // default PULSE
    uint32_t rgb1;                // 0xRRGGBB, default 0xFFFFFF
    uint32_t rgb2;                // 0xRRGGBB, default 0x000000
    uint32_t period_ms;           // default 1000
};

}
```

内部状態は上記 `Status` 相当の値を `SemaphoreHandle_t`（FreeRTOS mutex）で保護して
保持します。加えて、現在モードの開始時刻 `mode_start_ms` を保持します。

### 4.2 公開 API

```cpp
void start();                                                    // 初期化 + LED タスク起動
void set_lighting(Pattern p, uint32_t rgb1, uint32_t rgb2,
                  uint32_t period_ms);                           // led set。active へ遷移
void set_max_brightness(uint8_t value);                          // led max
void set_dimmer_brightness(DimmerTarget target, uint8_t percent);// led dim
void set_dimmer_time(TimeTarget target, uint32_t seconds);       // led time
void set_mode(Mode mode);                                        // led mode（強制遷移）
Status get_status();                                             // led status
void handle_button(button::Event event);                         // ボタンイベント反映
```

- `set_lighting()` は引数をすべて検証してから一括反映する。反映後は
  `mode = ACTIVE` にし、`mode_start_ms` をリセットする。
- `set_max_brightness()` / `set_dimmer_brightness()` / `set_dimmer_time()` は
  値の反映のみでモード遷移しない。
- `set_mode()` は任意のモードへ遷移し、`mode_start_ms` をリセットする
  （notification 指定時は notification 時間経過で sleep に落ちる）。
- `get_status()` は mutex で保護したスナップショットを返す。

### 4.3 LED タスク

更新周期は **10ms** とします。`vTaskDelayUntil()` で固定周期を維持します。

```text
        +--------------------------------------------+
        |       10ms ごとに起床 (vTaskDelayUntil)      |
        +---------------------+----------------------+
                              v
                 +------------+------------+
                 | 設定をスナップショット取得    |
                 +------------+------------+
                              v
                 +------------+------------+
                 | 自動モード遷移を判定        |
                 | active  -> dimmer1        |
                 | dimmer1 -> dimmer2        |
                 | notification -> sleep     |
                 +------------+------------+
                              v
                 +------------+------------+
                 | t = (now_ms % period)    |
                 |           / period       |
                 +------------+------------+
                              v
                 +------------+------------+
                 | coef 計算 -> RGB1/RGB2    |
                 | alpha-blend             |
                 +------------+------------+
                              v
                 +------------+------------+
                 | 最大輝度 / dimmer% を乗算 |
                 +------------+------------+
                              v
                 +------------+------------+
                 | 全 LED へ set_pixel       |
                 | + led_strip_refresh      |
                 +--------------------------+
```

- `now_ms` は `esp_timer_get_time() / 1000` を使用します。
- `mode == SLEEP` の場合は `led_strip_clear()` のみ実行します。
- 複数 LED（AtomS3 Lite の 4 灯）には同じ値を書きます。

### 4.4 パターン係数と輝度計算

`coef` の計算は要件どおりです（[led-control-requirements.md](led-control-requirements.md) の
「6. パターン仕様」参照）。実装では `cosf()` を使用し、浮動小数点で計算します。

```text
blend後チャンネル = RGB1チャンネル * coef + RGB2チャンネル * (1.0 - coef)
出力チャンネル    = blend後チャンネル * max_brightness / 255 * dimmer% / 100
```

- 整数への変換は四捨五入（`+0.5f` して切り捨て）とします。
- dimmer% は現在モードから決定します（active / notification は active 設定値）。

### 4.5 モード遷移とタイマー

```text
自動遷移:
  active        -- dimmer1 秒経過 --> dimmer1
  dimmer1       -- dimmer2 秒経過 --> dimmer2
  notification  -- notification 秒経過 --> sleep

イベント遷移:
  led set                         : 任意 -> active（タイマーリセット）
  led mode <mode>                 : 任意 -> 指定モード（タイマーリセット）
  ボタン                          : 要件の遷移表どおり
```

- モード遷移は必ず `mode_start_ms = now_ms` をセットします。
- `led set` / `led mode` / ボタンイベントは setter 経由でモードとタイマーを
  同時に更新します（mutex 内で一括更新）。

### 4.6 排他制御

```text
  uart_cli タスク -----> setter / getter -----+
                                              +---> [settings mutex] ---> LED タスクが参照
  button タスク -------> handle_button -------+
```

- 設定値の読み書きはすべて同じ mutex で保護します。
- LED タスクは毎 tick の先頭でスナップショットを取得し、`led_strip` 操作中は
  ロックを保持しません。

## 5. UI 設計（button / uart_cli）

ボタン入力と UART CLI の詳細設計は [led-control-ui.md](led-control-ui.md) に分離しています。

## 6. `main.cpp`

```cpp
extern "C" void app_main(void)
{
    llbeacon::led_control::start();
    llbeacon::button::start();
    llbeacon::uart_cli::uart_cli_start();
}
```

起動直後は `led_control` のデフォルト値（モード = active、パターン = PULSE、
RGB1 = `FFFFFF`、RGB2 = `000000`、周期 = 1000ms）で点灯を開始します。

## 7. 検証

### 7.1 ビルド

```sh
pio run -e m5stack-atoms3 -e m5stack-atom
```

両環境のビルド成功を必須とします。

### 7.2 実機確認項目

`pio device monitor -e <env>` で以下を確認します。

| # | 操作 | 期待結果 |
| --- | --- | --- |
| 1 | `led status` / `led status --json` | デフォルト値が正しく表示される。JSON が 1 行でパース可能 |
| 2 | `led set pulse FF0000 0000FF 2000` | active に遷移し、赤->青のパルス点滅（2 秒周期） |
| 3 | `led set flash FF0000 0000FF 2000` | 赤 -> 青へ sine 遷移 -> 青維持、を繰り返す |
| 4 | `led set saw FF0000 0000FF 2000` | 赤から青へ線形に変化 |
| 5 | `led set sine FF0000 0000FF 2000` | 赤 -> 青 -> 赤 を滑らかに往復 |
| 6 | `led max 64` / `led dim dimmer1 40` | モードが変わらず、明るさだけ変わる |
| 7 | `led time dimmer1 2` | 2 秒後に active -> dimmer1 へ自動遷移 |
| 8 | `led mode sleep` | 消灯する |
| 9 | sleep 中にボタン短押 | notification で一時点灯し、5 秒後に sleep へ戻る |
| 10 | dimmer1 / dimmer2 中にボタン短押 | active へ復帰する |
| 11 | active / dimmer1 / dimmer2 中にボタン長押 | sleep へ遷移する |
| 12 | 不正な引数（`led set pulse XYZ 0000FF 2000` 等） | `ERR: ...` が表示され、設定が変わらない |
| 13 | AtomS3 Lite（4 灯）と Atom Lite（1 灯） | すべての LED が同じ表示になる |

## 8. 実装上の注意・リスク

- **LED チップ差**: 既存コードは `LED_MODEL_WS2812` + GRB で初期化しており、
  SK6812 と互換で動作しています。この設定は維持します。
- **浮動小数点**: `cosf()` は ESP-IDF の libm で利用可能です。LED タスクは
  10ms 周期のため計算負荷は問題になりません。
- **スタックサイズ**: LED タスク 4096 バイト、button タスク 2048 バイト、
  CLI タスクは既存の 4096 バイトを目安とします。
- **`led set` の原子性**: パースと反映を分離し、全引数の検証が終わってから
  一度だけ設定を更新します。
