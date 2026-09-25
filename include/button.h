#pragma once

namespace llbeacon {
namespace button {

/**
 * @brief ボタンイベント種別
 */
enum class Event { SHORT_PRESS, LONG_PRESS };

/**
 * @brief ボタン入力(GPIO 割り込み)を初期化し、短押/長押を判定する FreeRTOS タスクを起動する
 *
 * 判定結果は led_control::handle_button() へ通知される。
 * app_main() から一度だけ呼び出すこと。
 */
void start();

}  // namespace button
}  // namespace llbeacon
