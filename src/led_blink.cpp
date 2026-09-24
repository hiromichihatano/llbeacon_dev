#include "led_blink.h"

#include <atomic>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "led_strip.h"
#include "llbeacon_board.h"

#define LED_BLINK_PERIOD_MS 500
#define LED_BRIGHTNESS 16
#define LED_BLINK_TASK_STACK_SIZE 2048
#define LED_BLINK_TASK_PRIORITY (tskIDLE_PRIORITY + 1)

static led_strip_handle_t rgb_led;
static TaskHandle_t led_blink_task_handle;
static std::atomic<bool> led_blink_enabled{true};

/**
 * @brief RGB LEDストリップを指定した状態(点灯/消灯)に設定する
 *
 * @param on true で全LEDを赤色に点灯、false で消灯する
 */
static void set_leds(bool on)
{
    if (on) {
        for (uint32_t index = 0; index < LLBEACON_RGB_LED_COUNT; index++) {
            ESP_ERROR_CHECK(led_strip_set_pixel(rgb_led, index, LED_BRIGHTNESS, 0, 0));
        }
        ESP_ERROR_CHECK(led_strip_refresh(rgb_led));
        return;
    }

    ESP_ERROR_CHECK(led_strip_clear(rgb_led));
}

/**
 * @brief LED点滅を行うFreeRTOSタスク本体
 *
 * led_blink_enabled が false の間はLEDを消灯した状態でタスク通知を待って
 * スリープし、true になると led_blink_set_enabled() からの通知で起床して
 * 点滅処理を再開する。
 *
 * @param arg 未使用
 */
static void led_blink_task(void *arg)
{
    (void)arg;

    while (true) {
        if (!led_blink_enabled.load()) {
            set_leds(false);
            // Sleep until led_blink_set_enabled(true) wakes this task up.
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        set_leds(true);
        vTaskDelay(pdMS_TO_TICKS(LED_BLINK_PERIOD_MS));
        set_leds(false);
        vTaskDelay(pdMS_TO_TICKS(LED_BLINK_PERIOD_MS));
    }
}

/** @copydoc led_blink_start */
void led_blink_start(void)
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
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &rgb_led));
    ESP_ERROR_CHECK(led_strip_clear(rgb_led));

    xTaskCreate(led_blink_task, "led_blink", LED_BLINK_TASK_STACK_SIZE, nullptr, LED_BLINK_TASK_PRIORITY,
                &led_blink_task_handle);
}

/** @copydoc led_blink_set_enabled */
void led_blink_set_enabled(bool enabled)
{
    const bool was_enabled = led_blink_enabled.exchange(enabled);
    if (enabled && !was_enabled && led_blink_task_handle != nullptr) {
        xTaskNotifyGive(led_blink_task_handle);
    }
}

