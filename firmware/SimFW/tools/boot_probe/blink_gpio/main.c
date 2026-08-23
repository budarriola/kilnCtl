/* boot_probe/blink_gpio -- minimal bare-metal blink, no FreeRTOS, no
 * TinyUSB, no SimFW code. Toggles GPIO25 (the onboard LED on a bare
 * Raspberry Pi Pico, NOT a Pico W) at ~1 Hz (500 ms on, 500 ms off -- "twice
 * a second" toggle rate). See ../README.md for what question this answers.
 */

#include "pico/stdlib.h"

#define LED_PIN 25

int main(void) {
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    while (true) {
        gpio_put(LED_PIN, 1);
        sleep_ms(500);
        gpio_put(LED_PIN, 0);
        sleep_ms(500);
    }

    return 0;
}
