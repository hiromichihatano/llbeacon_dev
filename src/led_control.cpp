#include "led_control.h"

#include <cassert>
#include <cmath>
#include <cstdint>

#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "llbeacon_board.h"

namespace llbeacon {
namespace led_control {

#define LED_UPDATE_PERIOD_MS 10
#define LED_TASK_STACK_SIZE 4096
#define LED_TASK_PRIORITY (tskIDLE_PRIORITY + 1)

#define LED_MIN_PERIOD_MS 100
#define LED_MAX_PERIOD_MS 60000
#define LED_MIN_TIME_S 1
#define LED_MAX_TIME_S 86400

static constexpr float kPi = 3.14159265358979f;

struct Settings {
    Mode mode;
    uint8_t max_brightness;
    uint8_t dimmer_active;
    uint8_t dimmer_dimmer1;
    uint8_t dimmer_dimmer2;
    uint32_t time_dimmer1_s;
    uint32_t time_dimmer2_s;
    uint32_t time_notification_s;
    Pattern pattern;
    uint32_t rgb1;
    uint32_t rgb2;
    uint32_t period_ms;
    uint64_t mode_start_ms;
};

static led_strip_handle_t rgb_led;
static SemaphoreHandle_t settings_mutex;
static TaskHandle_t led_task_handle;
static Settings settings = {
    .mode = Mode::ACTIVE,
    .max_brightness = 128,
    .dimmer_active = 100,
    .dimmer_dimmer1 = 30,
    .dimmer_dimmer2 = 10,
    .time_dimmer1_s = 10,
    .time_dimmer2_s = 180,
    .time_notification_s = 5,
    .pattern = Pattern::PULSE,
    .rgb1 = 0xFF0000,
    .rgb2 = 0x000000,
    .period_ms = 1000,
    .mode_start_ms = 0,
};

/**
 * @brief 現在時刻をミリ秒単位で取得する
 *
 * @return 起動からの経過時間(ms)
 */
static uint64_t current_time_ms(void)
{
    return static_cast<uint64_t>(esp_timer_get_time() / 1000LL);
}

/**
 * @brief 設定値保護用ミューテックスを取得する
 */
static void lock_settings(void)
{
    xSemaphoreTake(settings_mutex, portMAX_DELAY);
}

/**
 * @brief 設定値保護用ミューテックスを解放する
 */
static void unlock_settings(void)
{
    xSemaphoreGive(settings_mutex);
}

/**
 * @brief 指定されたモードへ遷移し、モード開始時刻をリセットする
 *
 * 呼び出し側で settings_mutex を保持していること。
 *
 * @param mode 遷移先モード
 */
static void transition_to_locked(Mode mode)
{
    settings.mode = mode;
    settings.mode_start_ms = current_time_ms();
}

/**
 * @brief モードごとの dimmer 輝度%を取得する
 *
 * @param mode 対象モード
 * @param s    設定スナップショット
 * @return dimmer 輝度%(0-100)
 */
static uint8_t dimmer_percent(Mode mode, const Settings &s)
{
    switch (mode) {
    case Mode::ACTIVE:
    case Mode::NOTIFICATION:
        return s.dimmer_active;
    case Mode::DIMMER1:
        return s.dimmer_dimmer1;
    case Mode::DIMMER2:
        return s.dimmer_dimmer2;
    case Mode::SLEEP:
        return 0;
    }
    return 0;
}

/**
 * @brief パターンと t から RGB1 係数を求める
 *
 * @param pattern パターン
 * @param t       周期内位置(0.0 <= t < 1.0)
 * @return RGB1 係数(0.0-1.0)
 */
static float pattern_coef(Pattern pattern, float t)
{
    switch (pattern) {
    case Pattern::PULSE:
        return t < 0.5f ? 1.0f : 0.0f;
    case Pattern::FLASH:
        if (t < 0.1f) {
            return 1.0f;
        }
        if (t < 0.3f) {
            return 0.5f * (1.0f + std::cos(kPi * (t - 0.1f) / 0.2f));
        }
        return 0.0f;
    case Pattern::SAW:
        return 1.0f - t;
    case Pattern::SINE:
        return 0.5f * (1.0f + std::cos(2.0f * kPi * t));
    }
    return 1.0f;
}

/**
 * @brief 1 チャンネル分の出力値を計算する
 *
 * @param c1        RGB1 のチャンネル値
 * @param c2        RGB2 のチャンネル値
 * @param coef      RGB1 係数
 * @param max_scale 最大輝度スケール(0.0-1.0)
 * @param dimmer    dimmer 輝度スケール(0.0-1.0)
 * @return LED へ出力する値(0-255)
 */
static uint32_t scale_channel(uint8_t c1, uint8_t c2, float coef, float max_scale, float dimmer)
{
    const float blended = static_cast<float>(c1) * coef + static_cast<float>(c2) * (1.0f - coef);
    const float scaled = blended * max_scale * dimmer;
    if (scaled <= 0.0f) {
        return 0;
    }
    if (scaled >= 255.0f) {
        return 255;
    }
    return static_cast<uint32_t>(scaled + 0.5f);
}

/**
 * @brief 現在の設定で LED を描画する
 *
 * @param s      設定スナップショット
 * @param now_ms 現在時刻(ms)
 */
static void render(const Settings &s, uint64_t now_ms)
{
    if (s.mode == Mode::SLEEP) {
        ESP_ERROR_CHECK(led_strip_clear(rgb_led));
        return;
    }

    const float t = static_cast<float>(now_ms % s.period_ms) / static_cast<float>(s.period_ms);
    const float coef = pattern_coef(s.pattern, t);
    const float max_scale = static_cast<float>(s.max_brightness) / 255.0f;
    const float dimmer = static_cast<float>(dimmer_percent(s.mode, s)) / 100.0f;

    const uint8_t r1 = (s.rgb1 >> 16) & 0xFF;
    const uint8_t g1 = (s.rgb1 >> 8) & 0xFF;
    const uint8_t b1 = s.rgb1 & 0xFF;
    const uint8_t r2 = (s.rgb2 >> 16) & 0xFF;
    const uint8_t g2 = (s.rgb2 >> 8) & 0xFF;
    const uint8_t b2 = s.rgb2 & 0xFF;

    const uint32_t r = scale_channel(r1, r2, coef, max_scale, dimmer);
    const uint32_t g = scale_channel(g1, g2, coef, max_scale, dimmer);
    const uint32_t b = scale_channel(b1, b2, coef, max_scale, dimmer);

    for (uint32_t index = 0; index < LLBEACON_RGB_LED_COUNT; index++) {
        ESP_ERROR_CHECK(led_strip_set_pixel(rgb_led, index, r, g, b));
    }
    ESP_ERROR_CHECK(led_strip_refresh(rgb_led));
}

/**
 * @brief 時間経過による自動モード遷移を適用する
 *
 * 呼び出し側で settings_mutex を保持していること。
 *
 * @param now_ms 現在時刻(ms)
 */
static void apply_automatic_transitions_locked(uint64_t now_ms)
{
    switch (settings.mode) {
    case Mode::ACTIVE:
        if (now_ms - settings.mode_start_ms >= static_cast<uint64_t>(settings.time_dimmer1_s) * 1000ULL) {
            transition_to_locked(Mode::DIMMER1);
        }
        break;
    case Mode::DIMMER1:
        if (now_ms - settings.mode_start_ms >= static_cast<uint64_t>(settings.time_dimmer2_s) * 1000ULL) {
            transition_to_locked(Mode::DIMMER2);
        }
        break;
    case Mode::NOTIFICATION:
        if (now_ms - settings.mode_start_ms >= static_cast<uint64_t>(settings.time_notification_s) * 1000ULL) {
            transition_to_locked(Mode::SLEEP);
        }
        break;
    default:
        break;
    }
}

/**
 * @brief LED 描画を行う FreeRTOS タスク本体
 *
 * 10ms ごとに自動モード遷移を判定してから LED を描画する。
 *
 * @param arg 未使用
 */
static void led_control_task(void *arg)
{
    (void)arg;

    TickType_t last_wake = xTaskGetTickCount();
    while (true) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(LED_UPDATE_PERIOD_MS));

        const uint64_t now_ms = current_time_ms();
        Settings snapshot;

        lock_settings();
        apply_automatic_transitions_locked(now_ms);
        snapshot = settings;
        unlock_settings();

        render(snapshot, now_ms);
    }
}

/** @copydoc start */
void start(void)
{
    const led_strip_config_t strip_config = {
        .strip_gpio_num = LLBEACON_RGB_LED_GPIO,
        .max_leds = LLBEACON_RGB_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {.invert_out = 0},
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags = {.with_dma = false},
    };

    settings_mutex = xSemaphoreCreateMutex();
    assert(settings_mutex != nullptr);

    settings.mode_start_ms = current_time_ms();

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &rgb_led));
    ESP_ERROR_CHECK(led_strip_clear(rgb_led));

    xTaskCreate(led_control_task, "led_control", LED_TASK_STACK_SIZE, nullptr, LED_TASK_PRIORITY,
                &led_task_handle);
}

/** @copydoc set_lighting */
void set_lighting(Pattern pattern, uint32_t rgb1, uint32_t rgb2, uint32_t period_ms)
{
    if (rgb1 > 0xFFFFFF || rgb2 > 0xFFFFFF || period_ms < LED_MIN_PERIOD_MS || period_ms > LED_MAX_PERIOD_MS) {
        return;
    }

    lock_settings();
    settings.pattern = pattern;
    settings.rgb1 = rgb1;
    settings.rgb2 = rgb2;
    settings.period_ms = period_ms;
    transition_to_locked(Mode::ACTIVE);
    unlock_settings();
}

/** @copydoc set_max_brightness */
void set_max_brightness(uint8_t value)
{
    lock_settings();
    settings.max_brightness = value;
    unlock_settings();
}

/** @copydoc set_dimmer_brightness */
void set_dimmer_brightness(DimmerTarget target, uint8_t percent)
{
    if (percent > 100) {
        return;
    }

    lock_settings();
    switch (target) {
    case DimmerTarget::ACTIVE:
        settings.dimmer_active = percent;
        break;
    case DimmerTarget::DIMMER1:
        settings.dimmer_dimmer1 = percent;
        break;
    case DimmerTarget::DIMMER2:
        settings.dimmer_dimmer2 = percent;
        break;
    }
    unlock_settings();
}

/** @copydoc set_dimmer_time */
void set_dimmer_time(TimeTarget target, uint32_t seconds)
{
    if (seconds < LED_MIN_TIME_S || seconds > LED_MAX_TIME_S) {
        return;
    }

    lock_settings();
    switch (target) {
    case TimeTarget::DIMMER1:
        settings.time_dimmer1_s = seconds;
        break;
    case TimeTarget::DIMMER2:
        settings.time_dimmer2_s = seconds;
        break;
    case TimeTarget::NOTIFICATION:
        settings.time_notification_s = seconds;
        break;
    }
    unlock_settings();
}

/** @copydoc set_mode */
void set_mode(Mode mode)
{
    lock_settings();
    transition_to_locked(mode);
    unlock_settings();
}

/** @copydoc get_status */
Status get_status(void)
{
    Status status;

    lock_settings();
    status.mode = settings.mode;
    status.max_brightness = settings.max_brightness;
    status.dimmer_active = settings.dimmer_active;
    status.dimmer_dimmer1 = settings.dimmer_dimmer1;
    status.dimmer_dimmer2 = settings.dimmer_dimmer2;
    status.time_dimmer1_s = settings.time_dimmer1_s;
    status.time_dimmer2_s = settings.time_dimmer2_s;
    status.time_notification_s = settings.time_notification_s;
    status.pattern = settings.pattern;
    status.rgb1 = settings.rgb1;
    status.rgb2 = settings.rgb2;
    status.period_ms = settings.period_ms;
    unlock_settings();

    return status;
}

/** @copydoc handle_button */
void handle_button(button::Event event)
{
    lock_settings();

    switch (event) {
    case button::Event::SHORT_PRESS:
        if (settings.mode == Mode::SLEEP) {
            transition_to_locked(Mode::NOTIFICATION);
        } else if (settings.mode == Mode::DIMMER1 || settings.mode == Mode::DIMMER2) {
            transition_to_locked(Mode::ACTIVE);
        }
        break;
    case button::Event::LONG_PRESS:
        if (settings.mode == Mode::SLEEP) {
            transition_to_locked(Mode::ACTIVE);
        } else if (settings.mode == Mode::ACTIVE || settings.mode == Mode::DIMMER1 || settings.mode == Mode::DIMMER2) {
            transition_to_locked(Mode::SLEEP);
        }
        break;
    }

    unlock_settings();
}

}  // namespace led_control
}  // namespace llbeacon
