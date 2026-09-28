#include "uart_cli.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "driver/uart.h"
#include "esp_log.h"
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

#include "audio_tone.h"
#include "embedded_cli.h"
#include "led_control.h"

namespace llbeacon {
namespace uart_cli {

#define UART_CLI_PORT UART_NUM_0
#define UART_CLI_BAUD_RATE 115200
#define UART_CLI_RX_BUFFER_SIZE 256
#define UART_CLI_TX_BUFFER_SIZE 0
#define UART_CLI_EVENT_QUEUE_SIZE 10
#define UART_CLI_TASK_STACK_SIZE 8192
#define UART_CLI_TASK_PRIORITY (tskIDLE_PRIORITY + 1)
#define UART_CLI_RX_CHUNK_SIZE 64

// サンプルレート 16kHz に対し上限 8000Hz は Nyquist 周波数そのもの。
// square / saw は高域ほど歪んで聞こえる点に注意（詳細は README 参照）。
#define TONE_FREQ_MIN_HZ 0
#define TONE_FREQ_MAX_HZ 8000
#define TONE_DURATION_MIN_MS 10
#define TONE_DURATION_MAX_MS 5000
#define TONE_VOLUME_MIN 0
#define TONE_VOLUME_MAX 100
#define TONE_DEFAULT_VOLUME 80

#define SOUND_VOLUME_MIN 0
#define SOUND_VOLUME_MAX 100

#if !UART_CLI_USE_USB_SERIAL_JTAG
static QueueHandle_t uart_event_queue;
#endif
static EmbeddedCli *cli;
static const char *TAG = "uart_cli";

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
 * std::from_chars で文字列全体を数値へ変換する。空白や符号付きの
 * 先頭は受け付けない（CLI トークンでは想定していないため）。
 *
 * @param token パース対象文字列
 * @param min   最小値
 * @param max   最大値
 * @param out   パース結果の格納先
 * @return 成功なら true
 */
static bool parse_u32(std::string_view token, uint32_t min, uint32_t max, uint32_t *out)
{
    uint32_t value = 0;
    const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
    if (ec != std::errc() || ptr != token.data() + token.size() || value < min || value > max) {
        return false;
    }
    *out = value;
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
 * @brief 名前と列挙値の組のテーブルから列挙値をパースする
 *
 * @param token 対象文字列
 * @param table 名前と列挙値の組の配列
 * @param out   パース結果の格納先
 * @return 成功なら true
 */
template <typename Enum, size_t N>
static bool parse_enum(const char *token, const std::pair<std::string_view, Enum> (&table)[N], Enum *out)
{
    if (token == nullptr) {
        return false;
    }
    for (const auto &[name, value] : table) {
        if (name == token) {
            *out = value;
            return true;
        }
    }
    return false;
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
    static constexpr std::pair<std::string_view, llbeacon::led_control::Pattern> kTable[] = {
        {"pulse", llbeacon::led_control::Pattern::PULSE},
        {"flash", llbeacon::led_control::Pattern::FLASH},
        {"saw", llbeacon::led_control::Pattern::SAW},
        {"sine", llbeacon::led_control::Pattern::SINE},
    };
    return parse_enum(token, kTable, out);
}

/**
 * @brief 波形名文字列をパースする
 *
 * @param token 波形名(sine / square / saw)
 * @param out   パース結果の格納先
 * @return 成功なら true
 */
static bool parse_waveform(const char *token, llbeacon::audio_tone::Waveform *out)
{
    static constexpr std::pair<std::string_view, llbeacon::audio_tone::Waveform> kTable[] = {
        {"sine", llbeacon::audio_tone::Waveform::SINE},
        {"square", llbeacon::audio_tone::Waveform::SQUARE},
        {"saw", llbeacon::audio_tone::Waveform::SAW},
    };
    return parse_enum(token, kTable, out);
}

/**
 * @brief note 文字列（周波数:ミリ秒[:音量]）をパースする
 *
 * 音量を省略した場合は TONE_DEFAULT_VOLUME を使う。
 *
 * @param token note 文字列
 * @param out   パース結果の格納先
 * @return 成功なら true
 */
static bool parse_note(const char *token, llbeacon::audio_tone::Note *out)
{
    if (token == nullptr) {
        return false;
    }

    // "周波数:ミリ秒[:音量]" を ':' で3分割する。
    const std::string_view text(token);
    const size_t first = text.find(':');
    if (first == std::string_view::npos) {
        return false;
    }
    const std::string_view rest = text.substr(first + 1);
    const size_t second = rest.find(':');
    const std::string_view freq = text.substr(0, first);
    const std::string_view duration = rest.substr(0, second);  // npos なら残り全体
    const std::string_view volume =
        second == std::string_view::npos ? std::string_view{} : rest.substr(second + 1);

    uint32_t freq_hz = 0;
    uint32_t duration_ms = 0;
    uint32_t volume_value = TONE_DEFAULT_VOLUME;
    if (!parse_u32(freq, TONE_FREQ_MIN_HZ, TONE_FREQ_MAX_HZ, &freq_hz) ||
        !parse_u32(duration, TONE_DURATION_MIN_MS, TONE_DURATION_MAX_MS, &duration_ms) ||
        (!volume.empty() && !parse_u32(volume, TONE_VOLUME_MIN, TONE_VOLUME_MAX, &volume_value))) {
        return false;
    }

    out->frequency_hz = freq_hz;
    out->duration_ms = duration_ms;
    out->volume = static_cast<uint8_t>(volume_value);
    return true;
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
    static constexpr std::pair<std::string_view, llbeacon::led_control::Mode> kTable[] = {
        {"active", llbeacon::led_control::Mode::ACTIVE},
        {"dimmer1", llbeacon::led_control::Mode::DIMMER1},
        {"dimmer2", llbeacon::led_control::Mode::DIMMER2},
        {"sleep", llbeacon::led_control::Mode::SLEEP},
        {"notification", llbeacon::led_control::Mode::NOTIFICATION},
    };
    return parse_enum(token, kTable, out);
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
    static constexpr std::pair<std::string_view, llbeacon::led_control::DimmerTarget> kTable[] = {
        {"active", llbeacon::led_control::DimmerTarget::ACTIVE},
        {"dimmer1", llbeacon::led_control::DimmerTarget::DIMMER1},
        {"dimmer2", llbeacon::led_control::DimmerTarget::DIMMER2},
    };
    return parse_enum(token, kTable, out);
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
    static constexpr std::pair<std::string_view, llbeacon::led_control::TimeTarget> kTable[] = {
        {"dimmer1", llbeacon::led_control::TimeTarget::DIMMER1},
        {"dimmer2", llbeacon::led_control::TimeTarget::DIMMER2},
        {"notification", llbeacon::led_control::TimeTarget::NOTIFICATION},
    };
    return parse_enum(token, kTable, out);
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
 * @brief 6桁ゼロ埋め大文字16進表記を指示するためのラッパー
 *
 * append_format の {} に渡すと "00FFAA" のように整形される。
 */
struct Hex06 {
    uint32_t value;  //!< 整形対象の値
};

/**
 * @brief 固定長バッファへ文字列を追記する
 *
 * 容量を超える場合は NUL 終界の範囲内で打ち切る（バッファオーバーフロー防止）。
 *
 * @param buf      出力バッファ
 * @param capacity buf の容量(バイト)
 * @param used     現在の使用長(バイト)。追記した分だけ進む
 * @param text     追記する文字列
 * @return 打ち切ったなら true
 */
static bool append_one(char *buf, size_t capacity, size_t *used, std::string_view text)
{
    const size_t room = capacity - *used - 1;  // NUL 終端用に 1 バイト残す
    const size_t count = std::min(text.size(), room);
    text.copy(buf + *used, count);
    *used += count;
    buf[*used] = '\0';
    return text.size() > room;
}

/**
 * @brief 固定長バッファへ10進整数を追記する
 *
 * std::to_chars で変換した結果を append_one(std::string_view) に委譲する。
 *
 * @param buf      出力バッファ
 * @param capacity buf の容量(バイト)
 * @param used     現在の使用長(バイト)。追記した分だけ進む
 * @param value    追記する整数
 * @return 打ち切ったなら true
 */
template <typename T> requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
static bool append_one(char *buf, size_t capacity, size_t *used, T value)
{
    char tmp[24];  // int64_t / uint64_t の最大桁数+符号に十分
    const auto [ptr, ec] = std::to_chars(tmp, tmp + sizeof(tmp), value);
    if (ec != std::errc()) {
        return false;
    }
    return append_one(buf, capacity, used, std::string_view(tmp, static_cast<size_t>(ptr - tmp)));
}

/**
 * @brief 固定長バッファへ Hex06 を6桁ゼロ埋め大文字16進で追記する
 *
 * @param buf      出力バッファ
 * @param capacity buf の容量(バイト)
 * @param used     現在の使用長(バイト)。追記した分だけ進む
 * @param hex      整形対象
 * @return 打ち切ったなら true
 */
static bool append_one(char *buf, size_t capacity, size_t *used, Hex06 hex)
{
    char tmp[8];  // uint32_t の16進最大桁数に一致
    const auto [ptr, ec] = std::to_chars(tmp, tmp + sizeof(tmp), hex.value, 16);
    if (ec != std::errc()) {
        return false;
    }
    for (char *p = tmp; p != ptr; p++) {
        if (*p >= 'a' && *p <= 'f') {
            *p = static_cast<char>(*p - 'a' + 'A');
        }
    }
    bool truncated = false;
    const size_t digits = static_cast<size_t>(ptr - tmp);
    for (size_t i = digits; i < 6; i++) {
        truncated |= append_one(buf, capacity, used, "0");
    }
    truncated |= append_one(buf, capacity, used, std::string_view(tmp, digits));
    return truncated;
}

/**
 * @brief フォーマット文字列のリテラル部を次の {} まで追記する
 *
 * "{{" と "}}" はそれぞれ "{" と "}" のエスケープとして扱う。
 * {} を見つけた時点で呼び出し元に戻り、引数の追記は呼び出し元が行う。
 *
 * @param buf      出力バッファ
 * @param capacity buf の容量(バイト)
 * @param used     現在の使用長(バイト)。追記した分だけ進む
 * @param fmt      フォーマット文字列
 * @param pos      fmt の走査位置。{} の直後まで進む
 * @return 打ち切ったなら true
 */
static bool append_literal(char *buf, size_t capacity, size_t *used, std::string_view fmt, size_t *pos)
{
    bool truncated = false;
    size_t start = *pos;
    while (*pos < fmt.size()) {
        const char c = fmt[*pos];
        const bool doubled = (*pos + 1 < fmt.size()) && (fmt[*pos + 1] == c);
        if ((c == '{' || c == '}') && doubled) {
            // "{{" / "}}" はエスケープされた括弧1文字。
            truncated |= append_one(buf, capacity, used, fmt.substr(start, *pos - start));
            truncated |= append_one(buf, capacity, used, std::string_view(&c, 1));
            *pos += 2;
            start = *pos;
            continue;
        }
        if (c == '{') {
            // "{}" がプレースホルダ。直前までのリテラルを追記して戻る。
            truncated |= append_one(buf, capacity, used, fmt.substr(start, *pos - start));
            *pos += 2;
            return truncated;
        }
        *pos += 1;
    }
    truncated |= append_one(buf, capacity, used, fmt.substr(start));
    return truncated;
}

/**
 * @brief 固定長バッファへフォーマット文字列と引数を追記する
 *
 * プレースホルダは {} のみ対応。整数は10進、Hex06 は6桁ゼロ埋め大文字16進、
 * それ以外は文字列として追記する。容量を超える場合は NUL 終端を保ったまま
 * 打ち切る（バッファオーバーフロー防止）。
 *
 * @param buf      出力バッファ
 * @param capacity buf の容量(バイト)
 * @param used     現在の使用長(バイト)。追記した分だけ進む
 * @param fmt      フォーマット文字列
 * @param args     プレースホルダへ順に埋める引数
 * @return 打ち切ったなら true
 */
template <typename... Args>
static bool append_format(char *buf, size_t capacity, size_t *used,
                          std::string_view fmt, Args &&...args)
{
    bool truncated = false;
    size_t pos = 0;
    // 引数ゼロの呼び出しでは fold が空になりラムダが未使用となるため抑制する。
    [[maybe_unused]] auto append_arg = [&](auto &&arg) {
        truncated |= append_literal(buf, capacity, used, fmt, &pos);
        truncated |= append_one(buf, capacity, used, std::forward<decltype(arg)>(arg));
    };
    (append_arg(args), ...);
    truncated |= append_literal(buf, capacity, used, fmt, &pos);
    return truncated;
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
    size_t used = 0;
    const bool truncated = append_format(
        buf, sizeof(buf), &used,
        "mode: {}\r\n"
        "max_brightness: {}\r\n"
        "dimmer.active: {}\r\n"
        "dimmer.dimmer1: {}\r\n"
        "dimmer.dimmer2: {}\r\n"
        "dimmer.sleep: 0\r\n"
        "time.dimmer1_s: {}\r\n"
        "time.dimmer2_s: {}\r\n"
        "time.notification_s: {}\r\n"
        "pattern: {}\r\n"
        "rgb1: {}\r\n"
        "rgb2: {}\r\n"
        "period_ms: {}",
        mode_name(status.mode), static_cast<unsigned>(status.max_brightness),
        static_cast<unsigned>(status.dimmer_active), static_cast<unsigned>(status.dimmer_dimmer1),
        static_cast<unsigned>(status.dimmer_dimmer2), status.time_dimmer1_s, status.time_dimmer2_s,
        status.time_notification_s, pattern_name(status.pattern), Hex06{status.rgb1},
        Hex06{status.rgb2}, status.period_ms);
    if (truncated) {
        ESP_LOGW(TAG, "led status text truncated");
    }
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
    size_t used = 0;
    const bool truncated = append_format(
        buf, sizeof(buf), &used,
        "{{\"mode\":\"{}\",\"max_brightness\":{},"
        "\"dimmer\":{{\"active\":{},\"dimmer1\":{},\"dimmer2\":{},\"sleep\":0}},"
        "\"time\":{{\"dimmer1_s\":{},\"dimmer2_s\":{},\"notification_s\":{}}},"
        "\"pattern\":\"{}\",\"rgb1\":\"{}\",\"rgb2\":\"{}\",\"period_ms\":{}}}",
        mode_name(status.mode), static_cast<unsigned>(status.max_brightness),
        static_cast<unsigned>(status.dimmer_active), static_cast<unsigned>(status.dimmer_dimmer1),
        static_cast<unsigned>(status.dimmer_dimmer2), status.time_dimmer1_s, status.time_dimmer2_s,
        status.time_notification_s, pattern_name(status.pattern), Hex06{status.rgb1},
        Hex06{status.rgb2}, status.period_ms);
    if (truncated) {
        ESP_LOGW(TAG, "led status json truncated");
    }
    embeddedCliPrint(embedded_cli, buf);
}

/**
 * @brief "sound" コマンドのヘルプ文字列を出力する
 *
 * @param embedded_cli 出力先のCLIインスタンス
 */
static void print_sound_help(EmbeddedCli *embedded_cli)
{
    embeddedCliPrint(embedded_cli,
                     "Usage:\r\n"
                     "  sound volume master <0-100>\r\n"
                     "  sound status [--json]\r\n"
                     "  sound stop");
}

/**
 * @brief "sound status" を human-readable 形式で出力する
 *
 * @param embedded_cli 出力先のCLIインスタンス
 * @param status       現在状態のスナップショット
 */
static void print_sound_status_text(EmbeddedCli *embedded_cli, const llbeacon::audio_tone::Status &status)
{
    if (!status.supported) {
        embeddedCliPrint(embedded_cli, "supported: false");
        return;
    }

    char buf[256];
    size_t used = 0;
    const bool truncated = append_format(
        buf, sizeof(buf), &used,
        "supported: true\r\n"
        "queued: {}\r\n"
        "master_volume: {}\r\n"
        "waveform: {}\r\n"
        "last_note: {}:{}:{}",
        status.queued, static_cast<unsigned>(status.master_volume),
        llbeacon::audio_tone::waveform_name(status.waveform), status.last_note.frequency_hz,
        status.last_note.duration_ms, static_cast<unsigned>(status.last_note.volume));
    if (truncated) {
        ESP_LOGW(TAG, "sound status text truncated");
    }
    embeddedCliPrint(embedded_cli, buf);
}

/**
 * @brief "sound status --json" を JSON 形式で出力する
 *
 * @param embedded_cli 出力先のCLIインスタンス
 * @param status       現在状態のスナップショット
 */
static void print_sound_status_json(EmbeddedCli *embedded_cli, const llbeacon::audio_tone::Status &status)
{
    if (!status.supported) {
        embeddedCliPrint(embedded_cli, "{\"supported\":false}");
        return;
    }

    char buf[320];
    size_t used = 0;
    const bool truncated = append_format(
        buf, sizeof(buf), &used,
        "{{\"supported\":true,\"queued\":{},\"master_volume\":{},\"waveform\":\"{}\","
        "\"last_note\":{{\"frequency_hz\":{},\"duration_ms\":{},\"volume\":{}}}}}",
        status.queued, static_cast<unsigned>(status.master_volume),
        llbeacon::audio_tone::waveform_name(status.waveform), status.last_note.frequency_hz,
        status.last_note.duration_ms, static_cast<unsigned>(status.last_note.volume));
    if (truncated) {
        ESP_LOGW(TAG, "sound status json truncated");
    }
    embeddedCliPrint(embedded_cli, buf);
}

/**
 * @brief "tone" コマンドのヘルプ文字列を出力する
 *
 * @param embedded_cli 出力先のCLIインスタンス
 */
static void print_tone_help(EmbeddedCli *embedded_cli)
{
    embeddedCliPrint(embedded_cli,
                     "Usage:\r\n"
                     "  tone <sine|square|saw> <freq:duration[:volume]> [<freq:duration[:volume]> ...]\r\n"
                     "  freq: 0-8000 (0=silence), duration: 10-5000 ms, volume: 0-100 (default 80)");
}

/**
 * @brief "tone" コマンドのバインディング関数
 *
 * waveform と 1 個以上の note（周波数:ミリ秒[:音量]）をパースし、
 * audio_tone へ note 毎に再生を要求する。
 *
 * @param embedded_cli 呼び出し元のCLIインスタンス
 * @param args         トークン化済みの引数文字列
 * @param context      未使用のコンテキストポインタ
 */
static void tone_command_binding(EmbeddedCli *embedded_cli, char *args, void *context)
{
    (void)context;

    // token 1 が波形名、token 2 以降が note。
    const char *waveform_token = embeddedCliGetToken(args, 1);
    llbeacon::audio_tone::Waveform waveform;
    if (waveform_token == nullptr || !parse_waveform(waveform_token, &waveform)) {
        print_tone_help(embedded_cli);
        return;
    }

    if (!llbeacon::audio_tone::is_supported()) {
        embeddedCliPrint(embedded_cli, "ERR: sound is not supported on this board");
        return;
    }

    // token 位置は 1 始まりで、最後の token が token_count 番目になる。
    // index = 2..token_count が note 列。
    uint32_t note_count = 0;
    const uint16_t token_count = embeddedCliGetTokenCount(args);
    for (uint16_t index = 2; index <= token_count; index++) {
        const char *note_token = embeddedCliGetToken(args, index);
        if (note_token == nullptr) {
            break;
        }
        if (note_count >= llbeacon::audio_tone::kMaxNotes) {
            embeddedCliPrint(embedded_cli, "ERR: too many notes (max 16)");
            return;
        }
        llbeacon::audio_tone::Note note;
        if (!parse_note(note_token, &note)) {
            embeddedCliPrint(embedded_cli, "ERR: invalid note: ");
            embeddedCliPrint(embedded_cli, note_token);
            return;
        }
        // 再生中でも受け付け、キューへ積んで順に再生させる。
        if (!llbeacon::audio_tone::push(waveform, note)) {
            embeddedCliPrint(embedded_cli, "ERR: sound queue is full");
            return;
        }
        note_count++;
    }

    if (note_count == 0) {
        print_tone_help(embedded_cli);
        return;
    }
    embeddedCliPrint(embedded_cli, "OK");
}

/**
 * @brief サブコマンドハンドラの関数ポインタ型
 */
using CommandHandler = void (*)(EmbeddedCli *embedded_cli, char *args);

/**
 * @brief サブコマンド名とハンドラのテーブルでコマンドを振り分ける
 *
 * サブコマンド名が table に無ければ fallback のヘルプを出力する。
 *
 * @param embedded_cli CLIインスタンス
 * @param args         トークン化済みの引数文字列
 * @param table        サブコマンド名とハンドラの組の配列
 * @param fallback     サブコマンドが不明のときに出力する関数
 */
template <size_t N>
static void dispatch_command(EmbeddedCli *embedded_cli, char *args,
                             const std::pair<std::string_view, CommandHandler> (&table)[N],
                             void (*fallback)(EmbeddedCli *))
{
    const char *sub = embeddedCliGetToken(args, 1);
    if (sub != nullptr) {
        for (const auto &[name, handler] : table) {
            if (name == sub) {
                handler(embedded_cli, args);
                return;
            }
        }
    }
    fallback(embedded_cli);
}

/**
 * @brief "sound volume" サブコマンドを処理する
 *
 * @param embedded_cli CLIインスタンス
 * @param args         トークン化済みの引数文字列
 */
static void sound_volume_command(EmbeddedCli *embedded_cli, char *args)
{
    // master volume は永続設定なので、非対応ボードでは意味を持たない。
    if (!llbeacon::audio_tone::is_supported()) {
        embeddedCliPrint(embedded_cli, "ERR: sound is not supported on this board");
        return;
    }

    const char *target_token = embeddedCliGetToken(args, 2);
    const char *value_token = embeddedCliGetToken(args, 3);
    uint32_t value = 0;

    if (target_token == nullptr || value_token == nullptr ||
        std::strcmp(target_token, "master") != 0 ||
        !parse_u32(value_token, SOUND_VOLUME_MIN, SOUND_VOLUME_MAX, &value)) {
        embeddedCliPrint(embedded_cli, "ERR: usage: sound volume master <0-100>");
        return;
    }

    llbeacon::audio_tone::set_master_volume(static_cast<uint8_t>(value));
    embeddedCliPrint(embedded_cli, "OK");
}

/**
 * @brief "sound stop" サブコマンドを処理する
 *
 * @param embedded_cli CLIインスタンス
 * @param args         トークン化済みの引数文字列
 */
static void sound_stop_command(EmbeddedCli *embedded_cli, char *args)
{
    (void)args;
    // stop も audio_tone の状態を変えるので、非対応ボードではエラーにする。
    if (!llbeacon::audio_tone::is_supported()) {
        embeddedCliPrint(embedded_cli, "ERR: sound is not supported on this board");
        return;
    }
    llbeacon::audio_tone::stop();
    embeddedCliPrint(embedded_cli, "OK");
}

/**
 * @brief "sound status" サブコマンドを処理する
 *
 * @param embedded_cli CLIインスタンス
 * @param args         トークン化済みの引数文字列
 */
static void sound_status_command(EmbeddedCli *embedded_cli, char *args)
{
    // --json が付いていれば JSON、無ければ人間向けテキストで出力する。
    const char *option = embeddedCliGetToken(args, 2);
    if (option != nullptr && std::strcmp(option, "--json") != 0) {
        embeddedCliPrint(embedded_cli, "ERR: usage: sound status [--json]");
        return;
    }

    const llbeacon::audio_tone::Status status = llbeacon::audio_tone::get_status();
    if (option != nullptr) {
        print_sound_status_json(embedded_cli, status);
    } else {
        print_sound_status_text(embedded_cli, status);
    }
}

/**
 * @brief "sound" コマンドのバインディング関数
 *
 * サブコマンド名で対応するハンドラへ振り分ける。
 *
 * @param embedded_cli 呼び出し元のCLIインスタンス
 * @param args         トークン化済みの引数文字列
 * @param context      未使用のコンテキストポインタ
 */
static void sound_command_binding(EmbeddedCli *embedded_cli, char *args, void *context)
{
    (void)context;
    static constexpr std::pair<std::string_view, CommandHandler> kTable[] = {
        {"volume", sound_volume_command},
        {"stop", sound_stop_command},
        {"status", sound_status_command},
    };
    dispatch_command(embedded_cli, args, kTable, print_sound_help);
}

/**
 * @brief "led set" サブコマンドを処理する
 *
 * @param embedded_cli CLIインスタンス
 * @param args         トークン化済みの引数文字列
 */
static void led_set_command(EmbeddedCli *embedded_cli, char *args)
{
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
}

/**
 * @brief "led max" サブコマンドを処理する
 *
 * @param embedded_cli CLIインスタンス
 * @param args         トークン化済みの引数文字列
 */
static void led_max_command(EmbeddedCli *embedded_cli, char *args)
{
    const char *value_token = embeddedCliGetToken(args, 2);
    uint32_t value = 0;
    if (value_token == nullptr || !parse_u32(value_token, 0, 255, &value)) {
        embeddedCliPrint(embedded_cli, "ERR: usage: led max <0-255>");
        return;
    }

    llbeacon::led_control::set_max_brightness(static_cast<uint8_t>(value));
    embeddedCliPrint(embedded_cli, "OK");
}

/**
 * @brief "led dim" サブコマンドを処理する
 *
 * @param embedded_cli CLIインスタンス
 * @param args         トークン化済みの引数文字列
 */
static void led_dim_command(EmbeddedCli *embedded_cli, char *args)
{
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
}

/**
 * @brief "led time" サブコマンドを処理する
 *
 * @param embedded_cli CLIインスタンス
 * @param args         トークン化済みの引数文字列
 */
static void led_time_command(EmbeddedCli *embedded_cli, char *args)
{
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
}

/**
 * @brief "led mode" サブコマンドを処理する
 *
 * @param embedded_cli CLIインスタンス
 * @param args         トークン化済みの引数文字列
 */
static void led_mode_command(EmbeddedCli *embedded_cli, char *args)
{
    const char *mode_token = embeddedCliGetToken(args, 2);
    llbeacon::led_control::Mode mode;

    if (mode_token == nullptr || !parse_mode(mode_token, &mode)) {
        embeddedCliPrint(embedded_cli,
                         "ERR: usage: led mode <active|dimmer1|dimmer2|sleep|notification>");
        return;
    }

    llbeacon::led_control::set_mode(mode);
    embeddedCliPrint(embedded_cli, "OK");
}

/**
 * @brief "led status" サブコマンドを処理する
 *
 * @param embedded_cli CLIインスタンス
 * @param args         トークン化済みの引数文字列
 */
static void led_status_command(EmbeddedCli *embedded_cli, char *args)
{
    // --json が付いていれば JSON、無ければ人間向けテキストで出力する。
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
}

/**
 * @brief "led" コマンドのバインディング関数
 *
 * サブコマンド名で対応するハンドラへ振り分ける。
 *
 * @param embedded_cli 呼び出し元のCLIインスタンス
 * @param args         トークン化済みの引数文字列
 * @param context      未使用のコンテキストポインタ
 */
static void led_command_binding(EmbeddedCli *embedded_cli, char *args, void *context)
{
    (void)context;
    static constexpr std::pair<std::string_view, CommandHandler> kTable[] = {
        {"set", led_set_command},
        {"max", led_max_command},
        {"dim", led_dim_command},
        {"time", led_time_command},
        {"mode", led_mode_command},
        {"status", led_status_command},
    };
    dispatch_command(embedded_cli, args, kTable, print_led_help);
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

    CliCommandBinding tone_binding = {
        .name = "tone",
        .help = "tone <sine|square|saw> <freq:duration[:volume]> ...",
        .tokenizeArgs = true,
        .context = nullptr,
        .binding = tone_command_binding,
    };
    embeddedCliAddBinding(cli, tone_binding);

    CliCommandBinding sound_binding = {
        .name = "sound",
        .help = "sound <volume|status|stop> ...",
        .tokenizeArgs = true,
        .context = nullptr,
        .binding = sound_command_binding,
    };
    embeddedCliAddBinding(cli, sound_binding);

    cli->writeChar = cli_write_char;

    xTaskCreate(uart_cli_task, "uart_cli", UART_CLI_TASK_STACK_SIZE, nullptr, UART_CLI_TASK_PRIORITY, nullptr);
}

}  // namespace uart_cli
}  // namespace llbeacon
