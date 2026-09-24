#pragma once

/**
 * Initializes the onboard RGB LED strip and starts a FreeRTOS task that
 * blinks it red at a fixed interval.
 *
 * Must be called once, after the scheduler has not yet necessarily started
 * (safe to call from app_main()).
 */
void led_blink_start(void);
