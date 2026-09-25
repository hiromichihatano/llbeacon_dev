#pragma once

#include <cstdint>

#include "button.h"

namespace llbeacon {
namespace led_control {

/**
 * @brief dimmer モード
 *
 * active は光らせ方の変更直後、dimmer1 / dimmer2 は時間経過で暗くした状態、
 * sleep は消灯、notification は sleep 中の一時通知を表す。
 */
enum class Mode { ACTIVE, DIMMER1, DIMMER2, SLEEP, NOTIFICATION };

/**
 * @brief LED のパターン(光らせ方)
 */
enum class Pattern { PULSE, FLASH, SAW, SINE };

/**
 * @brief dimmer 輝度を設定する対象
 *
 * sleep は 0% 固定のため設定対象に含めない。
 */
enum class DimmerTarget { ACTIVE, DIMMER1, DIMMER2 };

/**
 * @brief dimmer 時間を設定する対象
 */
enum class TimeTarget { DIMMER1, DIMMER2, NOTIFICATION };

/**
 * @brief LED 制御の現在設定のスナップショット
 */
struct Status {
    Mode mode;                    //!< 現在の dimmer モード
    uint8_t max_brightness;       //!< 最大輝度 0-255
    uint8_t dimmer_active;        //!< active の dimmer 輝度%
    uint8_t dimmer_dimmer1;       //!< dimmer1 の dimmer 輝度%
    uint8_t dimmer_dimmer2;       //!< dimmer2 の dimmer 輝度%
    uint32_t time_dimmer1_s;      //!< active -> dimmer1 の時間(秒)
    uint32_t time_dimmer2_s;      //!< dimmer1 -> dimmer2 の時間(秒)
    uint32_t time_notification_s; //!< notification -> sleep の時間(秒)
    Pattern pattern;              //!< 現在のパターン
    uint32_t rgb1;                //!< RGB1 (0xRRGGBB)
    uint32_t rgb2;                //!< RGB2 (0xRRGGBB)
    uint32_t period_ms;           //!< パターン周期(ms)
};

/**
 * @brief LED 制御を初期化し、パターン描画とモード遷移を行う FreeRTOS タスクを起動する
 *
 * app_main() から一度だけ呼び出すこと。
 */
void start();

/**
 * @brief 光らせ方(パターン / RGB1 / RGB2 / 周期)を一括設定する
 *
 * 引数はすべて検証され、1 つでも不正なら何も変更しない。
 * 設定に成功した場合は active モードへ遷移し、dimmer タイマーをリセットする。
 *
 * @param pattern   パターン
 * @param rgb1      RGB1 (0xRRGGBB)
 * @param rgb2      RGB2 (0xRRGGBB)
 * @param period_ms 周期(100-60000ms)
 */
void set_lighting(Pattern pattern, uint32_t rgb1, uint32_t rgb2, uint32_t period_ms);

/**
 * @brief 最大輝度を設定する
 *
 * モード遷移は行わない。
 *
 * @param value 最大輝度(0-255)
 */
void set_max_brightness(uint8_t value);

/**
 * @brief dimmer 輝度%を設定する
 *
 * モード遷移は行わない。
 *
 * @param target  設定対象(active / dimmer1 / dimmer2)
 * @param percent 輝度%(0-100)
 */
void set_dimmer_brightness(DimmerTarget target, uint8_t percent);

/**
 * @brief dimmer 時間を設定する
 *
 * モード遷移は行わない。
 *
 * @param target  設定対象(dimmer1 / dimmer2 / notification)
 * @param seconds 時間(1-86400秒)
 */
void set_dimmer_time(TimeTarget target, uint32_t seconds);

/**
 * @brief dimmer モードを強制的に遷移させる
 *
 * 遷移先モードのタイマーを開始する。
 *
 * @param mode 遷移先モード
 */
void set_mode(Mode mode);

/**
 * @brief 現在の設定スナップショットを取得する
 *
 * @return 現在の設定
 */
Status get_status();

/**
 * @brief ボタンイベントを dimmer モード遷移に反映する
 *
 * @param event ボタンイベント(SHORT_PRESS / LONG_PRESS)
 */
void handle_button(button::Event event);

}  // namespace led_control
}  // namespace llbeacon
