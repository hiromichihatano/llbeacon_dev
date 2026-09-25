#pragma once

namespace llbeacon {
namespace uart_cli {

/**
 * @brief UART0(console/USBシリアルと共用)を割り込み駆動のRX処理付きで初期化し、
 *        ドライバのイベントキューから受信バイトを取り出してembedded-cliに
 *        渡すFreeRTOSタスクを起動する
 *
 * 対応コマンド:
 *   - `led set <pattern> <rgb1> <rgb2> <period_ms>` : 光らせ方を一括設定
 *   - `led max <0-255>`                              : 最大輝度を設定
 *   - `led dim <active|dimmer1|dimmer2> <0-100>`     : dimmer 輝度%を設定
 *   - `led time <dimmer1|dimmer2|notification> <秒>` : dimmer 時間を設定
 *   - `led mode <active|dimmer1|dimmer2|sleep|notification>` : 強制モード遷移
 *   - `led status [--json]`                          : 設定を表示
 *
 * @note app_main() から一度だけ呼び出すこと。
 */
void uart_cli_start(void);

}  // namespace uart_cli
}  // namespace llbeacon
