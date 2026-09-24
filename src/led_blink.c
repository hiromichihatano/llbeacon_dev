#include "led_blink.h"

#include <stdbool.h>
#include <stdint.h>

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

static void led_blink_task(void *arg)
{
    (void)arg;

    while (true) {
        set_leds(true);
        vTaskDelay(pdMS_TO_TICKS(LED_BLINK_PERIOD_MS));
        set_leds(false);
        vTaskDelay(pdMS_TO_TICKS(LED_BLINK_PERIOD_MS));
    }
}

void led_blink_start(void)
{
    const led_strip_config_t strip_config = {
        .strip_gpio_num = LLBEACON_RGB_LED_GPIO,
        .max_leds = LLBEACON_RGB_LED_COUNT,
    };
    const led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &rgb_led));
    ESP_ERROR_CHECK(led_strip_clear(rgb_led));

    xTaskCreate(led_blink_task, "led_blink", LED_BLINK_TASK_STACK_SIZE, NULL, LED_BLINK_TASK_PRIORITY, NULL);
}
