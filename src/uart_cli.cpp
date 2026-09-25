#include "uart_cli.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "llbeacon_board.h"

#if defined(LLBEACON_BOARD_ATOMS3_LITE)
#include "driver/usb_serial_jtag.h"
#define UART_CLI_USE_USB_SERIAL_JTAG 1
#else
#define UART_CLI_USE_USB_SERIAL_JTAG 0
#endif

#include "embedded_cli.h"
#include "led_control.h"

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

#if !UART_CLI_USE_USB_SERIAL_JTAG
static QueueHandle_t uart_event_queue;
#endif
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
#if UART_CLI_USE_USB_SERIAL_JTAG
    usb_serial_jtag_write_bytes(&c, 1, portMAX_DELAY);
#else
    uart_write_bytes(UART_CLI_PORT, &c, 1);
#endif
}

/**
 * @brief 10進整数文字列を範囲指定付きでパースする
 *
 * @param token パース対象文字列
 * @param min   最小値
 * @param max   最大値
 * @param out   パース結果の格納先
 * @return 成功なら true
 */
static bool parse_u32(const char *token, uint32_t min, uint32_t max, uint32_t *out)
{
    if (token == nullptr || *token == '\0') {
        return false;
    }

    errno = 0;
    char *end = nullptr;
    const unsigned long value = std::strtoul(token, &end, 10);
    if (errno != 0 || end == token || *end != '\0') {
        return false;
    }
    if (value < min || value > max) {
        return false;
    }

    *out = static_cast<uint32_t>(value);
    return true;
}

/**
 * @brief RRGGBB 形式の hex 文字列をパースする
 *
 * @param token パース対象文字列(6桁の hex)
 * @param out   パース結果(0xRRGGBB)の格納先
 * @return 成功なら true
 */
static bool parse_hex_rgb(const char *token, uint32_t *out)
{
    if (token == nullptr || std::strlen(token) != 6) {
        return false;
    }

    uint32_t value = 0;
    for (size_t i = 0; i < 6; i++) {
        const char c = token[i];
        value <<= 4;
        if (c >= '0' && c <= '9') {
            value |= static_cast<uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value |= static_cast<uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            value |= static_cast<uint32_t>(c - 'A' + 10);
        } else {
            return false;
        }
    }

    *out = value;
    return true;
}

/**
 * @brief パターン名文字列をパースする
 *
 * @param token パターン名(pulse / flash / saw / sine)
 * @param out   パース結果の格納先
 * @return 成功なら true
 */
static bool parse_pattern(const char *token, llbeacon::led_control::Pattern *out)
{
    if (token == nullptr) {
        return false;
    }
    if (std::strcmp(token, "pulse") == 0) {
        *out = llbeacon::led_control::Pattern::PULSE;
        return true;
    }
    if (std::strcmp(token, "flash") == 0) {
        *out = llbeacon::led_control::Pattern::FLASH;
        return true;
    }
    if (std::strcmp(token, "saw") == 0) {
        *out = llbeacon::led_control::Pattern::SAW;
        return true;
    }
    if (std::strcmp(token, "sine") == 0) {
        *out = llbeacon::led_control::Pattern::SINE;
        return true;
    }
    return false;
}

/**
 * @brief モード名文字列をパースする
 *
 * @param token モード名(active / dimmer1 / dimmer2 / sleep / notification)
 * @param out   パース結果の格納先
 * @return 成功なら true
 */
static bool parse_mode(const char *token, llbeacon::led_control::Mode *out)
{
    if (token == nullptr) {
        return false;
    }
    if (std::strcmp(token, "active") == 0) {
        *out = llbeacon::led_control::Mode::ACTIVE;
        return true;
    }
    if (std::strcmp(token, "dimmer1") == 0) {
        *out = llbeacon::led_control::Mode::DIMMER1;
        return true;
    }
    if (std::strcmp(token, "dimmer2") == 0) {
        *out = llbeacon::led_control::Mode::DIMMER2;
        return true;
    }
    if (std::strcmp(token, "sleep") == 0) {
        *out = llbeacon::led_control::Mode::SLEEP;
        return true;
    }
    if (std::strcmp(token, "notification") == 0) {
        *out = llbeacon::led_control::Mode::NOTIFICATION;
        return true;
    }
    return false;
}

/**
 * @brief dimmer 輝度の設定対象名をパースする
 *
 * @param token 対象名(active / dimmer1 / dimmer2)
 * @param out   パース結果の格納先
 * @return 成功なら true
 */
static bool parse_dimmer_target(const char *token, llbeacon::led_control::DimmerTarget *out)
{
    if (token == nullptr) {
        return false;
    }
    if (std::strcmp(token, "active") == 0) {
        *out = llbeacon::led_control::DimmerTarget::ACTIVE;
        return true;
    }
    if (std::strcmp(token, "dimmer1") == 0) {
        *out = llbeacon::led_control::DimmerTarget::DIMMER1;
        return true;
    }
    if (std::strcmp(token, "dimmer2") == 0) {
        *out = llbeacon::led_control::DimmerTarget::DIMMER2;
        return true;
    }
    return false;
}

/**
 * @brief dimmer 時間の設定対象名をパースする
 *
 * @param token 対象名(dimmer1 / dimmer2 / notification)
 * @param out   パース結果の格納先
 * @return 成功なら true
 */
static bool parse_time_target(const char *token, llbeacon::led_control::TimeTarget *out)
{
    if (token == nullptr) {
        return false;
    }
    if (std::strcmp(token, "dimmer1") == 0) {
        *out = llbeacon::led_control::TimeTarget::DIMMER1;
        return true;
    }
    if (std::strcmp(token, "dimmer2") == 0) {
        *out = llbeacon::led_control::TimeTarget::DIMMER2;
        return true;
    }
    if (std::strcmp(token, "notification") == 0) {
        *out = llbeacon::led_control::TimeTarget::NOTIFICATION;
        return true;
    }
    return false;
}

/**
 * @brief パターン名を表示用文字列へ変換する
 *
 * @param pattern パターン
 * @return 表示用文字列
 */
static const char *pattern_name(llbeacon::led_control::Pattern pattern)
{
    switch (pattern) {
    case llbeacon::led_control::Pattern::PULSE:
        return "PULSE";
    case llbeacon::led_control::Pattern::FLASH:
        return "FLASH";
    case llbeacon::led_control::Pattern::SAW:
        return "SAW";
    case llbeacon::led_control::Pattern::SINE:
        return "SINE";
    }
    return "UNKNOWN";
}

/**
 * @brief モード名を表示用文字列へ変換する
 *
 * @param mode モード
 * @return 表示用文字列
 */
static const char *mode_name(llbeacon::led_control::Mode mode)
{
    switch (mode) {
    case llbeacon::led_control::Mode::ACTIVE:
        return "active";
    case llbeacon::led_control::Mode::DIMMER1:
        return "dimmer1";
    case llbeacon::led_control::Mode::DIMMER2:
        return "dimmer2";
    case llbeacon::led_control::Mode::SLEEP:
        return "sleep";
    case llbeacon::led_control::Mode::NOTIFICATION:
        return "notification";
    }
    return "unknown";
}

/**
 * @brief "led" コマンドのヘルプ文字列を出力する
 *
 * @param embedded_cli 出力先のCLIインスタンス
 */
static void print_led_help(EmbeddedCli *embedded_cli)
{
    embeddedCliPrint(embedded_cli,
                     "Usage:\r\n"
                     "  led set <pulse|flash|saw|sine> <RRGGBB> <RRGGBB> <100-60000>\r\n"
                     "  led max <0-255>\r\n"
                     "  led dim <active|dimmer1|dimmer2> <0-100>\r\n"
                     "  led time <dimmer1|dimmer2|notification> <1-86400>\r\n"
                     "  led mode <active|dimmer1|dimmer2|sleep|notification>\r\n"
                     "  led status [--json]");
}

/**
 * @brief "led status" を human-readable 形式で出力する
 *
 * @param embedded_cli 出力先のCLIインスタンス
 * @param status       現在の設定スナップショット
 */
static void print_status_text(EmbeddedCli *embedded_cli, const llbeacon::led_control::Status &status)
{
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "mode: %s\r\n"
                  "max_brightness: %u\r\n"
                  "dimmer.active: %u\r\n"
                  "dimmer.dimmer1: %u\r\n"
                  "dimmer.dimmer2: %u\r\n"
                  "dimmer.sleep: 0\r\n"
                  "time.dimmer1_s: %lu\r\n"
                  "time.dimmer2_s: %lu\r\n"
                  "time.notification_s: %lu\r\n"
                  "pattern: %s\r\n"
                  "rgb1: %06lX\r\n"
                  "rgb2: %06lX\r\n"
                  "period_ms: %lu",
                  mode_name(status.mode), status.max_brightness, status.dimmer_active,
                  status.dimmer_dimmer1, status.dimmer_dimmer2,
                  static_cast<unsigned long>(status.time_dimmer1_s),
                  static_cast<unsigned long>(status.time_dimmer2_s),
                  static_cast<unsigned long>(status.time_notification_s),
                  pattern_name(status.pattern), static_cast<unsigned long>(status.rgb1),
                  static_cast<unsigned long>(status.rgb2), static_cast<unsigned long>(status.period_ms));
    embeddedCliPrint(embedded_cli, buf);
}

/**
 * @brief "led status --json" を JSON 形式で出力する
 *
 * @param embedded_cli 出力先のCLIインスタンス
 * @param status       現在の設定スナップショット
 */
static void print_status_json(EmbeddedCli *embedded_cli, const llbeacon::led_control::Status &status)
{
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "{\"mode\":\"%s\",\"max_brightness\":%u,"
                  "\"dimmer\":{\"active\":%u,\"dimmer1\":%u,\"dimmer2\":%u,\"sleep\":0},"
                  "\"time\":{\"dimmer1_s\":%lu,\"dimmer2_s\":%lu,\"notification_s\":%lu},"
                  "\"pattern\":\"%s\",\"rgb1\":\"%06lX\",\"rgb2\":\"%06lX\",\"period_ms\":%lu}",
                  mode_name(status.mode), status.max_brightness, status.dimmer_active,
                  status.dimmer_dimmer1, status.dimmer_dimmer2,
                  static_cast<unsigned long>(status.time_dimmer1_s),
                  static_cast<unsigned long>(status.time_dimmer2_s),
                  static_cast<unsigned long>(status.time_notification_s),
                  pattern_name(status.pattern), static_cast<unsigned long>(status.rgb1),
                  static_cast<unsigned long>(status.rgb2), static_cast<unsigned long>(status.period_ms));
    embeddedCliPrint(embedded_cli, buf);
}

/**
 * @brief "led" コマンドのバインディング関数
 *
 * サブコマンドを解釈して led_control の setter を呼び出す。
 *
 * @param embedded_cli 呼び出し元のCLIインスタンス
 * @param args         トークン化済みの引数文字列
 * @param context      未使用のコンテキストポインタ
 */
static void led_command_binding(EmbeddedCli *embedded_cli, char *args, void *context)
{
    (void)context;

    const char *sub = embeddedCliGetToken(args, 1);
    if (sub == nullptr) {
        print_led_help(embedded_cli);
        return;
    }

    if (std::strcmp(sub, "set") == 0) {
        const char *pattern_token = embeddedCliGetToken(args, 2);
        const char *rgb1_token = embeddedCliGetToken(args, 3);
        const char *rgb2_token = embeddedCliGetToken(args, 4);
        const char *period_token = embeddedCliGetToken(args, 5);

        llbeacon::led_control::Pattern pattern;
        uint32_t rgb1 = 0;
        uint32_t rgb2 = 0;
        uint32_t period_ms = 0;

        if (pattern_token == nullptr || rgb1_token == nullptr || rgb2_token == nullptr ||
            period_token == nullptr || !parse_pattern(pattern_token, &pattern) ||
            !parse_hex_rgb(rgb1_token, &rgb1) || !parse_hex_rgb(rgb2_token, &rgb2) ||
            !parse_u32(period_token, 100, 60000, &period_ms)) {
            embeddedCliPrint(embedded_cli, "ERR: usage: led set <pulse|flash|saw|sine> <RRGGBB> <RRGGBB> <100-60000>");
            return;
        }

        llbeacon::led_control::set_lighting(pattern, rgb1, rgb2, period_ms);
        embeddedCliPrint(embedded_cli, "OK");
        return;
    }

    if (std::strcmp(sub, "max") == 0) {
        const char *value_token = embeddedCliGetToken(args, 2);
        uint32_t value = 0;
        if (value_token == nullptr || !parse_u32(value_token, 0, 255, &value)) {
            embeddedCliPrint(embedded_cli, "ERR: usage: led max <0-255>");
            return;
        }

        llbeacon::led_control::set_max_brightness(static_cast<uint8_t>(value));
        embeddedCliPrint(embedded_cli, "OK");
        return;
    }

    if (std::strcmp(sub, "dim") == 0) {
        const char *target_token = embeddedCliGetToken(args, 2);
        const char *percent_token = embeddedCliGetToken(args, 3);
        llbeacon::led_control::DimmerTarget target;
        uint32_t percent = 0;

        if (target_token == nullptr || percent_token == nullptr ||
            !parse_dimmer_target(target_token, &target) || !parse_u32(percent_token, 0, 100, &percent)) {
            embeddedCliPrint(embedded_cli, "ERR: usage: led dim <active|dimmer1|dimmer2> <0-100>");
            return;
        }

        llbeacon::led_control::set_dimmer_brightness(target, static_cast<uint8_t>(percent));
        embeddedCliPrint(embedded_cli, "OK");
        return;
    }

    if (std::strcmp(sub, "time") == 0) {
        const char *target_token = embeddedCliGetToken(args, 2);
        const char *seconds_token = embeddedCliGetToken(args, 3);
        llbeacon::led_control::TimeTarget target;
        uint32_t seconds = 0;

        if (target_token == nullptr || seconds_token == nullptr ||
            !parse_time_target(target_token, &target) || !parse_u32(seconds_token, 1, 86400, &seconds)) {
            embeddedCliPrint(embedded_cli, "ERR: usage: led time <dimmer1|dimmer2|notification> <1-86400>");
            return;
        }

        llbeacon::led_control::set_dimmer_time(target, seconds);
        embeddedCliPrint(embedded_cli, "OK");
        return;
    }

    if (std::strcmp(sub, "mode") == 0) {
        const char *mode_token = embeddedCliGetToken(args, 2);
        llbeacon::led_control::Mode mode;

        if (mode_token == nullptr || !parse_mode(mode_token, &mode)) {
            embeddedCliPrint(embedded_cli,
                             "ERR: usage: led mode <active|dimmer1|dimmer2|sleep|notification>");
            return;
        }

        llbeacon::led_control::set_mode(mode);
        embeddedCliPrint(embedded_cli, "OK");
        return;
    }

    if (std::strcmp(sub, "status") == 0) {
        const char *option = embeddedCliGetToken(args, 2);
        if (option != nullptr && std::strcmp(option, "--json") != 0) {
            embeddedCliPrint(embedded_cli, "ERR: usage: led status [--json]");
            return;
        }

        const llbeacon::led_control::Status status = llbeacon::led_control::get_status();
        if (option != nullptr) {
            print_status_json(embedded_cli, status);
        } else {
            print_status_text(embedded_cli, status);
        }
        return;
    }

    print_led_help(embedded_cli);
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

#if UART_CLI_USE_USB_SERIAL_JTAG
    while (true) {
        const int bytes_read = usb_serial_jtag_read_bytes(rx_chunk, sizeof(rx_chunk), pdMS_TO_TICKS(10));
        if (bytes_read > 0) {
            for (int i = 0; i < bytes_read; i++) {
                embeddedCliReceiveChar(cli, static_cast<char>(rx_chunk[i]));
            }
            embeddedCliProcess(cli);
        }
    }
#else
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
#endif
}

/** @copydoc uart_cli_start */
void uart_cli_start(void)
{
#if UART_CLI_USE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t usb_serial_jtag_config = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 256,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_serial_jtag_config));
#else
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
#endif

    cli = embeddedCliNewDefault();

    CliCommandBinding led_binding = {
        .name = "led",
        .help = "led <set|max|dim|time|mode|status> ...",
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
