#include <stdbool.h>
#include <stdint.h>
#include "nrf_gpio.h"
#include "nrfx_pwm.h"
#include "nrfx_gpiote.h"
#include "app_timer.h"
#include "nrfx_clock.h"

#define LED0_PIN 6
#define LED1_PIN 8
#define LED2_PIN 41
#define LED3_PIN 12
#define BUTTON_PIN 38

#define PWM_CHANNELS       4
#define PWM_TOP_VALUE      1000
#define FADE_INTERVAL_MS   5
#define FADE_STEP          10

#define DEBOUNCE_MS        200
#define DOUBLE_CLICK_MS    600

// Прототипы функций
void pwm_init(void);
void button_init(void);
void pwm_update(void *p_context);
void pwm_toggle(void);
void debounce_timer_handler(void *p_context);
void double_click_timer_handler(void *p_context);
void button_handler(nrfx_gpiote_pin_t pin, nrf_gpiote_polarity_t action);

// Глобальные переменные
static nrfx_pwm_t m_pwm_instance = NRFX_PWM_INSTANCE(0);
static nrf_pwm_values_individual_t m_seq_values;
static int m_led_counts[PWM_CHANNELS] = {6, 5, 7, 7};
static volatile bool m_running = true;
static bool m_fading_in = true;
static uint16_t m_current_duty = 0;
static int m_current_led_index = 0;
static int m_current_blink_count = 0;

static bool m_button_blocked = false;
static bool m_first_click_detected = false;

APP_TIMER_DEF(pwm_timer);
APP_TIMER_DEF(debounce_timer);
APP_TIMER_DEF(double_click_timer);

int main(void) {

    nrfx_clock_init(NULL);
    nrfx_clock_lfclk_start();
    while(!nrfx_clock_lfclk_is_running());

    app_timer_init();
    pwm_init();
    button_init();

    while(1)
        __WFE();
}

void pwm_init(void) {
    nrfx_pwm_config_t config = NRFX_PWM_DEFAULT_CONFIG;
    config.output_pins[0] = LED0_PIN;
    config.output_pins[1] = LED1_PIN;
    config.output_pins[2] = LED2_PIN;
    config.output_pins[3] = LED3_PIN;
    config.base_clock = NRF_PWM_CLK_1MHz;
    config.count_mode = NRF_PWM_MODE_UP;
    config.top_value  = PWM_TOP_VALUE;
    config.load_mode  = NRF_PWM_LOAD_INDIVIDUAL;
    config.step_mode  = NRF_PWM_STEP_AUTO;

    nrfx_pwm_init(&m_pwm_instance, &config, NULL);

    m_seq_values.channel_0 = 0;
    m_seq_values.channel_1 = 0;
    m_seq_values.channel_2 = 0;
    m_seq_values.channel_3 = 0;

    app_timer_create(&pwm_timer, APP_TIMER_MODE_REPEATED, pwm_update);
    app_timer_start(pwm_timer, APP_TIMER_TICKS(FADE_INTERVAL_MS), NULL);
}

void pwm_update(void *p_context) {
    (void)p_context;
    if (!m_running) return;

    if (m_fading_in) {
        m_current_duty += FADE_STEP;
        if (m_current_duty >= PWM_TOP_VALUE)
        {
            m_current_duty = PWM_TOP_VALUE;
            m_fading_in = false;
        }
    }
    else {
        if (m_current_duty <= FADE_STEP) {
            m_current_duty = 0;
            m_fading_in = true;
            m_current_blink_count++;
            if (m_current_blink_count >= m_led_counts[m_current_led_index]) {
                m_current_blink_count = 0;
                do {
                    m_current_led_index = (m_current_led_index + 1) % PWM_CHANNELS;
                } while (m_led_counts[m_current_led_index] == 0);
            }
        }
        else {
            m_current_duty -= FADE_STEP;
        }
    }

    m_seq_values.channel_0 = 0;
    m_seq_values.channel_1 = 0;
    m_seq_values.channel_2 = 0;
    m_seq_values.channel_3 = 0;

    switch (m_current_led_index) {
        case 0: m_seq_values.channel_0 = m_current_duty; break;
        case 1: m_seq_values.channel_1 = m_current_duty; break;
        case 2: m_seq_values.channel_2 = m_current_duty; break;
        case 3: m_seq_values.channel_3 = m_current_duty; break;
    }

    nrf_pwm_sequence_t seq = {
        .values.p_individual = &m_seq_values,
        .length = PWM_CHANNELS,
        .repeats = 0,
        .end_delay = 0
    };

    nrfx_pwm_simple_playback(&m_pwm_instance, &seq, 1, 0);
}

void pwm_toggle(void) {
    m_running = !m_running;
    if (m_running) {
        app_timer_start(pwm_timer, APP_TIMER_TICKS(FADE_INTERVAL_MS), NULL);
    }
    else {
        app_timer_stop(pwm_timer);
        nrf_pwm_sequence_t seq = {
            .values.p_individual = &m_seq_values,
            .length = PWM_CHANNELS,
            .repeats = 0,
            .end_delay = 0
        };
        nrfx_pwm_simple_playback(&m_pwm_instance, &seq, 1, 0);
    }
}

void debounce_timer_handler(void *p_context) { m_button_blocked = false; }
void double_click_timer_handler(void *p_context) { m_first_click_detected = false; }

void button_handler(nrfx_gpiote_pin_t pin, nrf_gpiote_polarity_t action) {
    if (m_button_blocked) return;

    m_button_blocked = true;
    app_timer_start(debounce_timer, APP_TIMER_TICKS(DEBOUNCE_MS), NULL);

    if (!m_first_click_detected) {
        m_first_click_detected = true;
        app_timer_start(double_click_timer, APP_TIMER_TICKS(DOUBLE_CLICK_MS), NULL);
    }
    else {
        m_first_click_detected = false;
        app_timer_stop(double_click_timer);
        pwm_toggle();
    }
}

void button_init(void) {
    if (!nrfx_gpiote_is_init())
        nrfx_gpiote_init();

    nrf_gpio_cfg_input(BUTTON_PIN, NRF_GPIO_PIN_PULLUP);

    nrfx_gpiote_in_config_t in_cfg = NRFX_GPIOTE_CONFIG_IN_SENSE_HITOLO(true);
    in_cfg.pull = NRF_GPIO_PIN_PULLUP;

    nrfx_gpiote_in_init(BUTTON_PIN, &in_cfg, button_handler);
    nrfx_gpiote_in_event_enable(BUTTON_PIN, true);

    app_timer_create(&debounce_timer, APP_TIMER_MODE_SINGLE_SHOT, debounce_timer_handler);
    app_timer_create(&double_click_timer, APP_TIMER_MODE_SINGLE_SHOT, double_click_timer_handler);
}
