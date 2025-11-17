#include <stdbool.h>
#include <stdint.h>

#include "nrf_gpio.h"
#include "nrf_delay.h"
#include "nrfx_pwm.h"

#define LED0_PIN  NRF_GPIO_PIN_MAP(0,6)
#define LED1_PIN  NRF_GPIO_PIN_MAP(0,8)
#define LED2_PIN  NRF_GPIO_PIN_MAP(1,9)
#define LED3_PIN  NRF_GPIO_PIN_MAP(0,12)

#define BUTTON_PIN NRF_GPIO_PIN_MAP(1,6)

#define PWM_TOP_VALUE 255
#define FADE_STEPS    50

// PWM instance
nrfx_pwm_t m_pwm0 = NRFX_PWM_INSTANCE(0);

nrf_pwm_values_individual_t seq_values = {
    .channel_0 = 0,
    .channel_1 = 0,
    .channel_2 = 0,
    .channel_3 = 0
};

nrf_pwm_sequence_t pwm_seq = {
    .values.p_individual = &seq_values,
    .length = NRF_PWM_VALUES_LENGTH(seq_values),
    .repeats = 0,
    .end_delay = 0
};

int digits[] = { 6, 5, 7, 7 };
const int digits_count = 4;

void pwm_init(void)
{
    nrfx_pwm_config_t config;

    // Указываем пины
    config.output_pins[0] = LED0_PIN;
    config.output_pins[1] = LED1_PIN;
    config.output_pins[2] = LED2_PIN;
    config.output_pins[3] = LED3_PIN;

    // IRQ приоритет — можно любое значение 0..7
    config.irq_priority = 7;

    // Режимы PWM
    config.base_clock = NRF_PWM_CLK_1MHz;   // частота 1 MHz
    config.count_mode = NRF_PWM_MODE_UP;
    config.top_value = PWM_TOP_VALUE;
    config.load_mode = NRF_PWM_LOAD_INDIVIDUAL;
    config.step_mode = NRF_PWM_STEP_AUTO;

    nrfx_pwm_init(&m_pwm0, &config, NULL);
}

void button_init(void)
{
    nrf_gpio_cfg_input(BUTTON_PIN, NRF_GPIO_PIN_PULLUP);
}

bool button_pressed(void)
{
    return nrf_gpio_pin_read(BUTTON_PIN) == 0;
}

void set_pwm_duty(int led_index, uint16_t duty)
{
    switch (led_index) {
        case 0: seq_values.channel_0 = duty; break;
        case 1: seq_values.channel_1 = duty; break;
        case 2: seq_values.channel_2 = duty; break;
        case 3: seq_values.channel_3 = duty; break;
    }

    nrfx_pwm_simple_playback(&m_pwm0, &pwm_seq, 1, NRFX_PWM_FLAG_LOOP);
}

void fade_in(int led_index, int time_ms)
{
    int delay = time_ms / FADE_STEPS;

    for (int i = 0; i <= FADE_STEPS; i++)
    {
        uint16_t duty = (PWM_TOP_VALUE * i) / FADE_STEPS;
        set_pwm_duty(led_index, duty);
        nrf_delay_ms(delay);
    }
}

void fade_out(int led_index, int time_ms)
{
    int delay = time_ms / FADE_STEPS;

    for (int i = FADE_STEPS; i >= 0; i--)
    {
        uint16_t duty = (PWM_TOP_VALUE * i) / FADE_STEPS;
        set_pwm_duty(led_index, duty);
        nrf_delay_ms(delay);
    }
}

void blink(int led_index)
{
    fade_in(led_index, 200);
    fade_out(led_index, 200);
}

int main(void)
{
    pwm_init();
    button_init();

    int led_index = 0;
    int blink_done = 0;

    while (1)
    {
        while (!button_pressed()) {}

        while (button_pressed())
        {
            blink(led_index);
            blink_done++;

            if (blink_done >= digits[led_index])
            {
                led_index = (led_index + 1) % digits_count;
                blink_done = 0;
            }
        }
    }
}
