#include "uart_cli.h"

#include <cstring>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"

#include "embedded_cli.h"
#include "led_blink.h"

namespace llbeacon {
namespace uart_cli {

#define UART_CLI_PORT UART_NUM_0
#define UART_CLI_BAUD_RATE 115200
#define UART_CLI_RX_BUFFER_SIZE 256
#define UART_CLI_TX_BUFFER_SIZE 0
#define UART_CLI_EVENT_QUEUE_SIZE 10
#define UART_CLI_TASK_STACK_SIZE 4096
#define UART_CLI_TASK_PRIORITY (tskIDLE_PRIORITY + 1)
#define UART_CLI_RX_CHUNK_SIZE 64

static QueueHandle_t uart_event_queue;
static EmbeddedCli *cli;

/**
 * @brief embedded-cliが1文字出力するたびに呼ばれるコールバック
 *
 * 受け取った文字をそのままUART0へ書き込む。
 *
 * @param embedded_cli 呼び出し元のCLIインスタンス(未使用)
 * @param c            出力する1文字
 */
static void cli_write_char(EmbeddedCli *embedded_cli, char c)
{
    (void)embedded_cli;
    uart_write_bytes(UART_CLI_PORT, &c, 1);
}

/**
 * @brief "led" コマンドのバインディング関数
 *
 * 第1引数が "1" ならLED点滅を有効化、"0" なら無効化する。
 *
 * @param embedded_cli 呼び出し元のCLIインスタンス(未使用)
 * @param args         トークン化済みの引数文字列("0" または "1")
 * @param context      未使用のコンテキストポインタ
 */
static void led_command_binding(EmbeddedCli *embedded_cli, char *args, void *context)
{
    (void)embedded_cli;
    (void)context;

    const char *state = embeddedCliGetToken(args, 1);
    if (state != nullptr && std::strcmp(state, "1") == 0) {
        llbeacon::led_blink::led_blink_set_enabled(true);
    } else if (state != nullptr && std::strcmp(state, "0") == 0) {
        llbeacon::led_blink::led_blink_set_enabled(false);
    }
}

/**
 * @brief UART受信イベントをCLIへ橋渡しするFreeRTOSタスク本体
 *
 * uart_driver_install() が生成するイベントキューを待ち受け、UART_DATA
 * イベント受信時に受信バイトを読み出して embeddedCliReceiveChar() へ
 * 1バイトずつ渡し、embeddedCliProcess() でコマンド処理を行う。
 * バッファ溢れが発生した場合は受信バッファとキューをクリアして復旧する。
 *
 * @param arg 未使用
 */
static void uart_cli_task(void *arg)
{
    (void)arg;

    uint8_t rx_chunk[UART_CLI_RX_CHUNK_SIZE];
    uart_event_t event;

    while (true) {
        if (xQueueReceive(uart_event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (event.type) {
        case UART_DATA: {
            int bytes_read;
            while ((bytes_read = uart_read_bytes(UART_CLI_PORT, rx_chunk, sizeof(rx_chunk), 0)) > 0) {
                for (int i = 0; i < bytes_read; i++) {
                    embeddedCliReceiveChar(cli, static_cast<char>(rx_chunk[i]));
                }
            }
            embeddedCliProcess(cli);
            break;
        }
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
            uart_flush_input(UART_CLI_PORT);
            xQueueReset(uart_event_queue);
            break;
        default:
            break;
        }
    }
}

/** @copydoc uart_cli_start */
void uart_cli_start(void)
{
    const uart_config_t uart_config = {
        .baud_rate = UART_CLI_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .rx_glitch_filt_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {
            .allow_pd = 0,
            .backup_before_sleep = 0,
        },
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_CLI_PORT, UART_CLI_RX_BUFFER_SIZE, UART_CLI_TX_BUFFER_SIZE,
                                        UART_CLI_EVENT_QUEUE_SIZE, &uart_event_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_CLI_PORT, &uart_config));
    ESP_ERROR_CHECK(
        uart_set_pin(UART_CLI_PORT, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    cli = embeddedCliNewDefault();

    CliCommandBinding led_binding = {
        .name = "led",
        .help = "Usage: led <0|1> - disable/enable LED blinking",
        .tokenizeArgs = true,
        .context = nullptr,
        .binding = led_command_binding,
    };
    embeddedCliAddBinding(cli, led_binding);

    cli->writeChar = cli_write_char;

    xTaskCreate(uart_cli_task, "uart_cli", UART_CLI_TASK_STACK_SIZE, nullptr, UART_CLI_TASK_PRIORITY, nullptr);
}

}  // namespace uart_cli
}  // namespace llbeacon
