#include "button.h"

#include <cassert>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "led_control.h"
#include "llbeacon_board.h"

namespace llbeacon {
namespace button {

#define BUTTON_TASK_STACK_SIZE 2048
#define BUTTON_TASK_PRIORITY (tskIDLE_PRIORITY + 1)
#define BUTTON_EVENT_QUEUE_SIZE 8
#define BUTTON_DEBOUNCE_MS 50
#define BUTTON_LONG_PRESS_MS 1000

static QueueHandle_t button_event_queue;
static TaskHandle_t button_task_handle;

/**
 * @brief ボタンの両エッジ割り込みハンドラ
 *
 * 押下状態(pressed = LOW)をイベントキューへ送る。判定は button_task で行う。
 *
 * @param arg 未使用
 */
static void IRAM_ATTR button_isr(void *arg)
{
    (void)arg;

    const bool pressed = (gpio_get_level(LLBEACON_BUTTON_GPIO) == 0);
    BaseType_t higher_priority_task_woken = pdFALSE;
    xQueueSendFromISR(button_event_queue, &pressed, &higher_priority_task_woken);
    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/**
 * @brief ボタンエッジを判定する FreeRTOS タスク本体
 *
 * 押下開始(FALL)から 1000ms 経過で LONG_PRESS を即時発火し、
 * 1000ms 未満で離した場合は 50ms 以上なら SHORT_PRESS、50ms 未満は
 * チャタリングとして無視する。
 *
 * @param arg 未使用
 */
static void button_task(void *arg)
{
    (void)arg;

    while (true) {
        bool edge = false;

        // 押下開始(FALL)まで待つ。RISE は無視する。
        while (xQueueReceive(button_event_queue, &edge, portMAX_DELAY) == pdTRUE) {
            if (edge) {
                break;
            }
        }

        const TickType_t pressed_at = xTaskGetTickCount();

        while (true) {
            if (xQueueReceive(button_event_queue, &edge, pdMS_TO_TICKS(BUTTON_LONG_PRESS_MS)) == pdTRUE) {
                if (!edge) {
                    const uint32_t held_ms = pdTICKS_TO_MS(xTaskGetTickCount() - pressed_at);
                    if (held_ms >= BUTTON_DEBOUNCE_MS && held_ms < BUTTON_LONG_PRESS_MS) {
                        llbeacon::led_control::handle_button(Event::SHORT_PRESS);
                    }
                    break;
                }
                // 押下中のチャタリングによる重複 FALL は無視する。
            } else {
                llbeacon::led_control::handle_button(Event::LONG_PRESS);
                // 離す(RISE)まで待つ。
                while (xQueueReceive(button_event_queue, &edge, portMAX_DELAY) == pdTRUE) {
                    if (!edge) {
                        break;
                    }
                }
                break;
            }
        }
    }
}

/** @copydoc start */
void start(void)
{
    button_event_queue = xQueueCreate(BUTTON_EVENT_QUEUE_SIZE, sizeof(bool));
    assert(button_event_queue != nullptr);

    const gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << LLBEACON_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(LLBEACON_BUTTON_GPIO, button_isr, nullptr));

    xTaskCreate(button_task, "button", BUTTON_TASK_STACK_SIZE, nullptr, BUTTON_TASK_PRIORITY,
                &button_task_handle);
}

}  // namespace button
}  // namespace llbeacon
