#pragma once

namespace llbeacon {
namespace uart_cli {

/**
 * @brief UART0(console/USBシリアルと共用)を割り込み駆動のRX処理付きで初期化し、
 *        ドライバのイベントキューから受信バイトを取り出してembedded-cliに
 *        渡すFreeRTOSタスクを起動する
 *
 * 対応コマンド:
 *   - `led 0` : LED点滅を無効化(消灯)する
 *   - `led 1` : LED点滅を有効化する
 *
 * @note app_main() から一度だけ呼び出すこと。
 */
void uart_cli_start(void);

}  // namespace uart_cli
}  // namespace llbeacon
