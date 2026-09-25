# LED 制御 要件定義

本ドキュメントは [Issue #2](https://github.com/hiromichihatano/llbeacon_dev/issues/2) の
「仕様: LED の光らせ方について」を元に、確認で確定した要件（不足分・誤りの修正を含む）を
まとめたものです。実装方針は [led-control-design.md](led-control-design.md)、
UI（button / uart_cli）の設計は [led-control-ui.md](led-control-ui.md) を参照してください。

## 1. 概要

- 対象ハードウェア: M5Stack **AtomS3 Lite** / **Atom Lite** の内蔵 RGB LED
  - AtomS3 Lite: 4 灯 / Atom Lite: 1 灯（いずれも SK6812 系、WS2812 互換）
- UART CLI から LED の光らせ方を設定できる
- 時間経過とボタン操作により、明るさを段階的に落とす（dimmer）モード遷移を持つ
- 設定値は**永続化しない**。電源投入ごとにデフォルト値で起動する
- 既存の `led 0` / `led 1`（単純点滅）は廃止し、本仕様に置き換える

## 2. 用語

| 用語 | 意味 |
| --- | --- |
| パターン | LED の光らせ方（PULSE / FLASH / SAW / SINE） |
| RGB1 / RGB2 | パターンで行き来する 2 色。`RRGGBB` の 6 桁 hex で指定 |
| 周期 | パターン 1 周の時間（ms） |
| 最大輝度 | 全 LED 出力の上限値（0-255） |
| dimmer 輝度 | モードごとの明るさ（%、sleep は 0% 固定） |
| dimmer 時間 | モードが自動遷移するまでの時間（秒） |
| モード | active / dimmer1 / dimmer2 / sleep / notification の 5 状態 |
| 短押 / 長押 | ボタン押下時間 1000ms 未満 / 1000ms 以上 |

## 3. コマンド要件

| コマンド | 動作 | 範囲 / デフォルト |
| --- | --- | --- |
| `led set <pattern> <rgb1> <rgb2> <period_ms>` | 光らせ方を一括設定し **active へ遷移** | pattern: `pulse` / `flash` / `saw` / `sine`、rgb1 / rgb2: `RRGGBB`、period: 100-60000ms（default 1000） |
| `led max <0-255>` | 最大輝度を設定 | 0-255、default 128 |
| `led dim <active\|dimmer1\|dimmer2> <0-100>` | dimmer 輝度%を設定 | 0-100、default active 100 / dimmer1 80 / dimmer2 30 |
| `led time <dimmer1\|dimmer2\|notification> <秒>` | dimmer 時間を設定 | 1-86400、default dimmer1 10 / dimmer2 180 / notification 5 |
| `led mode <active\|dimmer1\|dimmer2\|sleep\|notification>` | 強制モード遷移 | 全 5 モード指定可、遷移先のタイマーを開始 |
| `led status [--json]` | 現在の設定を表示 | `--json` で JSON 1 行出力 |

- `led set` は 4 値を**原子的に**設定する。いずれか 1 つでも不正なら何も変更しない。
- `led set` 実行後は必ず `active` に遷移し、dimmer タイマーをリセットする。
- `led max` / `led dim` / `led time` は設定値のみ変更し、モードは遷移させない。
- `led status` はモード遷移タイマーに影響しない。

例:

```text
led set pulse FF0000 0000FF 2000
led dim dimmer1 50
led time dimmer1 10
led mode sleep
led status --json
```

## 4. 設定値とデフォルト

| 設定項目 | 範囲 | デフォルト | 備考 |
| --- | --- | --- | --- |
| 最大輝度 | 0-255 | **128** | SK6812 の連続点灯を考慮した保守値（約 50%） |
| dimmer 輝度 active | 0-100% | 100% | |
| dimmer 輝度 dimmer1 | 0-100% | 80% | |
| dimmer 輝度 dimmer2 | 0-100% | 30% | |
| dimmer 輝度 sleep | - | **0%（固定）** | 設定不可 |
| dimmer 時間 dimmer1 | 1-86400 秒 | 10 秒 | |
| dimmer 時間 dimmer2 | 1-86400 秒 | 180 秒 | |
| dimmer 時間 notification | 1-86400 秒 | 5 秒 | |
| パターン | PULSE / FLASH / SAW / SINE | PULSE | |
| RGB1 | `RRGGBB` | `FFFFFF` | |
| RGB2 | `RRGGBB` | `000000` | |
| 周期 | 100-60000ms | 1000ms | |
| 起動時モード | - | active | 電源投入直後 |

最大輝度 128（= 50%）は、SK6812 のチャンネルあたり約 12mA の定電流駆動を
PWM で 50% に絞る値です。フル輝度の長時間連続点灯は劣化リスクがあるため、
デフォルトは 50% とします。

## 5. LED 出力の計算

LED に実際に送る値は以下の式で計算します。

```text
出力チャンネル = blend後チャンネル * 最大輝度 / 255 * dimmer輝度% / 100
```

`blend後チャンネル` は、RGB1 係数 `coef` による alpha-blend です。

```text
blend後チャンネル = RGB1チャンネル * coef + RGB2チャンネル * (1.0 - coef)
```

モードごとに使用する dimmer 輝度% は以下のとおりです。

| モード | 使用する dimmer 輝度% |
| --- | --- |
| active | active 設定値（default 100%） |
| dimmer1 | dimmer1 設定値（default 80%） |
| dimmer2 | dimmer2 設定値（default 30%） |
| notification | **active 設定値**（確定事項） |
| sleep | 0%（消灯） |

## 6. パターン仕様

`t` は周期的に `0.0 -> 1.0` に変化し、`1.0` 到達後 `0.0` に戻ります。

```text
t = (現在時刻_ms % 周期_ms) / 周期_ms        （0.0 <= t < 1.0）
```

RGB1 係数 `coef` は以下のとおりです。

| パターン | t の区間 | coef（RGB1 係数） |
| --- | --- | --- |
| PULSE | 0.0 <= t < 0.5 | 1.0 |
| PULSE | 0.5 <= t < 1.0 | 0.0 |
| FLASH | 0.0 <= t < 0.1 | 1.0 |
| FLASH | 0.1 <= t < 0.3 | `0.5 * (1 + cos(pi * (t - 0.1) / 0.2))` |
| FLASH | 0.3 <= t < 1.0 | 0.0（RGB2 を維持） |
| SAW | 0.0 <= t < 1.0 | `1.0 - t` |
| SINE | 0.0 <= t < 1.0 | `0.5 * (1 + cos(2*pi*t))` |

> 元コメントの SINE の式 `0.5 * (sin(t*2pi) + 1.0)` は
> 「RGB1 -> RGB2 -> RGB1」という説明と一致しないため、
> `0.5 * (1 + cos(2*pi*t))` に修正しました。

パターンの時間変化を図にすると以下のようになります。

```text
t(%)    0         25        50        75        100
        |         |         |         |         |
PULSE:  |#########|#########|.........|.........|
FLASH:  |####~~~~~|~~.......|.........|.........|
SAW:    |RGB1~~~~~|~~~~~~~~~|~~~~~~~~~|~~~~~RGB2|
SINE:   |#######~~|~~~......|......~~~|~~#######|
```

凡例: `#` = RGB1、`.` = RGB2、`~` = 遷移/中間

## 7. Dimmer モード遷移

### 7.1 モード一覧

| モード | 意味 |
| --- | --- |
| active | 光らせ方（`led set`）変更直後。最も明るい |
| dimmer1 | 通知の継続発光（active から一定時間経過後） |
| dimmer2 | 長時間放置中（dimmer1 からさらに時間経過後） |
| sleep | 消灯中 |
| notification | 消灯中の一時通知 |

### 7.2 遷移表

| 現在モード | イベント | 遷移先 | 備考 |
| --- | --- | --- | --- |
| active | dimmer1 秒経過 | dimmer1 | 自動遷移 |
| dimmer1 | dimmer2 秒経過 | dimmer2 | 自動遷移 |
| dimmer1 / dimmer2 | `led set` 実行 | active | タイマーリセット |
| dimmer1 / dimmer2 | ボタン短押 | active | タイマーリセット |
| active / dimmer1 / dimmer2 | ボタン長押 | sleep | |
| sleep | ボタン短押 | notification | notification タイマー開始 |
| sleep | ボタン長押 | active | タイマーリセット |
| notification | notification 秒経過 | sleep | 自動遷移 |
| 任意 | `led mode <mode>` | 指定モード | 遷移先のタイマーを開始 |

- ボタン短押 / 長押の判定は「1000ms 未満 = 短押、1000ms 以上 = 長押」です。
- notification 中のボタン操作は無視します（notification 秒経過で sleep に戻ります）。
- `led max` / `led dim` / `led time` / `led status` はモード遷移を引き起こしません。

### 7.3 明るさの時間変化（概念図）

```text
                 dimmer1時間             dimmer2時間
active  |#########################|                         |
dimmer1 |                         |#########################|
dimmer2 |                         |                         |############...
        +-------------------------+-------------------------+----------------> 時間
```

- active は dimmer1 時間後に dimmer1、dimmer1 は dimmer2 時間後に dimmer2 へ自動遷移します。
- dimmer2 は自動では sleep に遷移しません（sleep へはボタン長押または `led mode sleep`）。
- sleep 中のボタン短押: sleep -> notification（active 輝度で一時点灯）-> notification 時間後に sleep。

## 8. ボタン操作

| 判定 | 条件 |
| --- | --- |
| 短押 | 押下時間 50ms 以上 1000ms 未満 |
| 長押 | 押下時間 1000ms 以上 |
| 無視 | 押下時間 50ms 未満（チャタリング） |

モードごとの動作:

```text
                SHORT                         LONG
active         無視                          sleep
dimmer1        active                        sleep
dimmer2        active                        sleep
sleep          notification                  active
notification   無視                          無視
```

## 9. エラー処理

- 範囲外の値、不正な hex、不正なモード名 / パターン名はエラーとして扱う。
- エラー時は `ERR: ...` を表示し、**設定値は変更しない**。
- `led set` は 4 引数のうち 1 つでも不正なら、4 つとも変更しない。

## 10. 制約・非機能要件

- 両ボード（`m5stack-atoms3` / `m5stack-atom`）でビルドできること。
- 複数 LED（AtomS3 Lite の 4 灯）はすべて同じ色・同じ明るさで点灯する。
- `led status --json` の出力はプログラムでパースしやすい JSON 1 行とする。
- 設定の永続化は行わない（NVS 不使用）。
- 既存の `led 0` / `led 1` コマンドと `led_blink` モジュールは廃止する。

## Appendix: 要件確定時の判断記録

| 論点 | 確定内容 |
| --- | --- |
| 設定の永続化 | 保存しない（毎回デフォルト値） |
| FLASH の 70-100% 区間 | RGB2 を維持（30-100% が RGB2） |
| notification の表示 | 現在のパターンを active 輝度で表示 |
| active 復帰条件 | `led set`（パターン / RGB1 / RGB2 / 周期）の変更時のみ |
| 強制 dimmer 遷移 | 全 5 モード指定可、遷移先のタイマーを開始 |
| `pattern` / `rgb1` / `rgb2` / `period` | 単体設定はしない。`led set` で同時設定 |
| 一括設定コマンド名 | `led set` |
| 既存 `led 0` / `led 1` | 新 LED エンジンに置き換えて削除 |
