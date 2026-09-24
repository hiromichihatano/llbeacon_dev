#pragma once

/**
 * Initializes the onboard RGB LED strip and starts a FreeRTOS task that
 * blinks it red at a fixed interval.
 *
 * Must be called once, after the scheduler has not yet necessarily started
 * (safe to call from app_main()).
 */
void led_blink_start(void);

/**
 * Enables or disables the LED blinking.
 * When disabled, the LED strip is immediately turned off and the blink task
 * stays idle until re-enabled.
 *
 * Safe to call from any task. Must be called after led_blink_start().
 */
void led_blink_set_enabled(bool enabled);
