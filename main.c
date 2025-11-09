#include <stdbool.h>
#include <stdint.h>
#include "nrf_gpio.h"
#include "nrf_delay.h"

#define LED0_PIN  NRF_GPIO_PIN_MAP(0,6)
#define LED1_PIN  NRF_GPIO_PIN_MAP(0,8)
#define LED2_PIN  NRF_GPIO_PIN_MAP(1,9)
#define LED3_PIN  NRF_GPIO_PIN_MAP(0,12)
#define BUTTON_PIN NRF_GPIO_PIN_MAP(1,6)

uint32_t leds[] = { LED0_PIN, LED1_PIN, LED2_PIN, LED3_PIN };
const int led_count = 4;
int digits[] = { 6, 5, 7, 7 };
const int digits_count = 4;

void blink(int);
void leds_init();
void led_on(int);
void led_off(int);
void button_init();
bool button_pressed();

int main() {
    leds_init();
    button_init();

    int led_index = 0;     
    int blink_done = 0;   

    while (1) {
        // Жду пока не будет нажата кнопка
        while (!button_pressed()) {}

        while (button_pressed()) {
            blink(led_index);
            blink_done++;

            if (blink_done >= digits[led_index]) {
                led_index = (led_index + 1) % digits_count;
                blink_done = 0;
            }
        }
    }
}

void blink(int led_index) {
    led_on(led_index);
    nrf_delay_ms(200);
    led_off(led_index);
    nrf_delay_ms(200);
}

void leds_init() {
    for (int i = 0; i < led_count; i++)
    {
        nrf_gpio_cfg_output(leds[i]);
        nrf_gpio_pin_write(leds[i], 1); // выкл
    }
}

void led_on(int index) {
    nrf_gpio_pin_write(leds[index], 0);
}

void led_off(int index) {
    nrf_gpio_pin_write(leds[index], 1);
}

void button_init() {
    nrf_gpio_cfg_input(BUTTON_PIN, NRF_GPIO_PIN_PULLUP);
}

bool button_pressed() {
    return nrf_gpio_pin_read(BUTTON_PIN) == 0;
}