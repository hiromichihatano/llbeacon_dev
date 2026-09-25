#pragma once

namespace llbeacon {
namespace led_blink {

/**
 * @brief オンボードRGB LEDストリップを初期化し、一定間隔で赤色に点滅させる
 *        FreeRTOSタスクを起動する
 *
 * app_main() から一度だけ呼び出すこと(スケジューラ開始前でも呼び出し可能)。
 */
void led_blink_start(void);

/**
 * @brief LED点滅の有効/無効を切り替える
 *
 * 無効化すると即座にLEDを消灯し、再度有効化されるまで点滅タスクは待機する。
 *
 * @param enabled true で点滅を有効化、false で無効化(消灯)
 *
 * @note どのタスクから呼び出しても安全。led_blink_start() の呼び出し後に
 *       使用すること。
 */
void led_blink_set_enabled(bool enabled);

}  // namespace led_blink
}  // namespace llbeacon
