/* boot_probe/blink_uart -- same minimal blink as ../blink_gpio/main.c, plus
 * a printed line over UART0 (GP16 TX / GP17 RX, 115200 8N1 -- same pins/baud
 * SimFW's own CMakeLists.txt configures, see this directory's
 * CMakeLists.txt) each toggle, via pico_enable_stdio_uart. Validates the
 * UART wiring and console path independently of SimFW. See ../README.md
 * for what question this answers.
 */

#include "pico/stdlib.h"
#include <stdio.h>

#define LED_PIN 25

int main(void) {
    stdio_init_all();

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    uint32_t count = 0;
    while (true) {
        gpio_put(LED_PIN, 1);
        printf("boot_probe/blink_uart: tick %lu (LED on)\r\n", (unsigned long)count);
        sleep_ms(500);

        gpio_put(LED_PIN, 0);
        printf("boot_probe/blink_uart: tick %lu (LED off)\r\n", (unsigned long)count);
        sleep_ms(500);

        count++;
    }

    return 0;
}
