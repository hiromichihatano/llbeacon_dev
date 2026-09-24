#pragma once

/**
 * Initializes UART0 (the same UART used for the console/USB-serial) with an
 * interrupt-driven RX path, and starts a FreeRTOS task that pulls received
 * bytes from the driver's event queue and feeds them into an embedded-cli
 * instance.
 *
 * Supported commands:
 *   led 0   - disable LED blinking (turns the LED off)
 *   led 1   - enable LED blinking
 *
 * Must be called once from app_main().
 */
void uart_cli_start(void);
