#include "led_blink.h"
#include "uart_cli.h"

/**
 * @brief アプリケーションのエントリポイント
 *
 * LED点滅タスクとUART CLIタスクを起動する。
 */
extern "C" void app_main(void)
{
    llbeacon::led_blink::led_blink_start();
    llbeacon::uart_cli::uart_cli_start();
}
