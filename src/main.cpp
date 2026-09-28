#include "audio_tone.h"
#include "button.h"
#include "led_control.h"
#include "uart_cli.h"

/**
 * @brief アプリケーションのエントリポイント
 *
 * LED 制御タスク、スピーカー、ボタン入力タスク、UART CLI タスクを起動する。
 */
extern "C" void app_main(void)
{
    llbeacon::led_control::start();
    llbeacon::audio_tone::start();
    llbeacon::button::start();
    llbeacon::uart_cli::uart_cli_start();
}
