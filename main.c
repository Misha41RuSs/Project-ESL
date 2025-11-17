#include <stdbool.h>
#include <stdint.h>
#include "nrf_gpio.h"
#include "nrfx_pwm.h"
#include "nrfx_gpiote.h"
#include "app_timer.h"
#include "nrfx_clock.h"

#define LED0_PIN  NRF_GPIO_PIN_MAP(0,6)
#define LED1_PIN  NRF_GPIO_PIN_MAP(0,8)
#define LED2_PIN  NRF_GPIO_PIN_MAP(1,9)
#define LED3_PIN  NRF_GPIO_PIN_MAP(0,12)

#define BUTTON_PIN NRF_GPIO_PIN_MAP(1,6)

#define PWM_TOP_VALUE      255
#define FADE_INTERVAL_MS   5
#define FADE_STEP          5
#define DEBOUNCE_MS        80
#define DOUBLE_CLICK_MS    450

APP_TIMER_DEF(pwm_timer);
APP_TIMER_DEF(debounce_timer);
APP_TIMER_DEF(double_click_timer);

nrfx_pwm_t m_pwm0 = NRFX_PWM_INSTANCE(0);
nrf_pwm_values_individual_t pwm_values;

static const int digits[4] = {6,5,7,7};
static const int digits_count = 4;

volatile bool running = true;
static bool fading_in = true;
static uint16_t current_duty = 0;
static int led_index = 0;
static int blink_count = 0;

static volatile bool button_blocked = false;
static volatile bool first_click_detected = false;

// ================= PWM =================

void set_pwm_duty(int led_idx, uint16_t duty) {
    pwm_values.channel_0 = (led_idx == 0) ? duty : 0;
    pwm_values.channel_1 = (led_idx == 1) ? duty : 0;
    pwm_values.channel_2 = (led_idx == 2) ? duty : 0;
    pwm_values.channel_3 = (led_idx == 3) ? duty : 0;

    nrf_pwm_sequence_t seq = {
        .values.p_individual = &pwm_values,
        .length = 4,
        .repeats = 0,
        .end_delay = 0
    };
    nrfx_pwm_simple_playback(&m_pwm0, &seq, 1, NRFX_PWM_FLAG_LOOP);
}

void pwm_timer_handler(void *p_context) {
    if (!running) return;

    // Плавное включение/выключение
    if (fading_in) {
        current_duty += FADE_STEP;
        if (current_duty >= PWM_TOP_VALUE) {
            current_duty = PWM_TOP_VALUE;
            fading_in = false;
        }
    } else {
        if (current_duty <= FADE_STEP) {
            current_duty = 0;
            fading_in = true;
            blink_count++;
            if (blink_count >= digits[led_index]) {
                blink_count = 0;
                led_index = (led_index + 1) % digits_count;
            }
        } else {
            current_duty -= FADE_STEP;
        }
    }

    set_pwm_duty(led_index, current_duty);
}

// ================= Button =================

void debounce_timer_handler(void *p_context) { button_blocked = false; }
void double_click_timer_handler(void *p_context) { first_click_detected = false; }

void button_handler(nrfx_gpiote_pin_t pin, nrf_gpiote_polarity_t action) {
    if (button_blocked) return;

    button_blocked = true;
    app_timer_start(debounce_timer, APP_TIMER_TICKS(DEBOUNCE_MS), NULL);

    if (!first_click_detected) {
        first_click_detected = true;
        app_timer_start(double_click_timer, APP_TIMER_TICKS(DOUBLE_CLICK_MS), NULL);
    } else {
        first_click_detected = false;
        app_timer_stop(double_click_timer);
        running = !running; // переключаем состояние цикла
    }
}

// ================= Init =================

void pwm_init(void) {
    nrfx_pwm_config_t cfg = NRFX_PWM_DEFAULT_CONFIG;
    cfg.output_pins[0] = LED0_PIN;
    cfg.output_pins[1] = LED1_PIN;
    cfg.output_pins[2] = LED2_PIN;
    cfg.output_pins[3] = LED3_PIN;
    cfg.base_clock = NRF_PWM_CLK_1MHz;
    cfg.count_mode = NRF_PWM_MODE_UP;
    cfg.top_value = PWM_TOP_VALUE;
    cfg.load_mode = NRF_PWM_LOAD_INDIVIDUAL;
    cfg.step_mode = NRF_PWM_STEP_AUTO;
    nrfx_pwm_init(&m_pwm0, &cfg, NULL);

    pwm_values.channel_0 = 0;
    pwm_values.channel_1 = 0;
    pwm_values.channel_2 = 0;
    pwm_values.channel_3 = 0;
    set_pwm_duty(0,0);
}

void button_init(void) {
    if (!nrfx_gpiote_is_init()) nrfx_gpiote_init();

    nrf_gpio_cfg_input(BUTTON_PIN, NRF_GPIO_PIN_PULLUP);

    nrfx_gpiote_in_config_t in_cfg = NRFX_GPIOTE_CONFIG_IN_SENSE_HITOLO(true);
    in_cfg.pull = NRF_GPIO_PIN_PULLUP;
    nrfx_gpiote_in_init(BUTTON_PIN, &in_cfg, button_handler);
    nrfx_gpiote_in_event_enable(BUTTON_PIN, true);

    app_timer_create(&debounce_timer, APP_TIMER_MODE_SINGLE_SHOT, debounce_timer_handler);
    app_timer_create(&double_click_timer, APP_TIMER_MODE_SINGLE_SHOT, double_click_timer_handler);
}

// ================= Main =================

int main(void) {
    nrfx_clock_init(NULL);
    nrfx_clock_lfclk_start();
    while(!nrfx_clock_lfclk_is_running());

    app_timer_init();

    pwm_init();
    button_init();

    app_timer_create(&pwm_timer, APP_TIMER_MODE_REPEATED, pwm_timer_handler);
    app_timer_start(pwm_timer, APP_TIMER_TICKS(FADE_INTERVAL_MS), NULL);

    while(1) {
        __WFE(); // ждем события, основной цикл свободен
    }
}
