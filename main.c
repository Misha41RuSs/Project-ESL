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

// Дебаунсинг и тайминги двойного нажатия
#define DEBOUNCE_DELAY_MS    50
#define DOUBLE_CLICK_MAX_MS  300

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

// Глобальные переменные состояния
bool running = true; // Флаг работы цикла свечения
uint32_t last_button_check_time = 0;
bool last_button_state = true;
uint32_t last_click_time = 0;
bool waiting_for_double_click = false;

void pwm_init(void);
void button_init(void);
bool check_button_double_click(void);
void set_pwm_duty(int, uint16_t);
void fade_in(int, int);
void fade_out(int, int);
void blink(int);

int digits[] = { 6, 5, 7, 7 };
const int digits_count = 4;

int main(void) {
    pwm_init();
    button_init();

    int led_index = 0;
    int blink_done = 0;

    while (1) {
        // Проверяем двойное нажатие
        if (check_button_double_click()) {
            running = !running;
            if (running) {
                // Сбрасываем состояние при возобновлении
                led_index = 0;
                blink_done = 0;
            }
        }

        if (running) {
            // Выполняем обычный цикл мигания
            blink(led_index);
            blink_done++;

            if (blink_done >= digits[led_index]) {
                led_index = (led_index + 1) % digits_count;
                blink_done = 0;
            }
        } else {
            // Если цикл остановлен, просто ждем
            nrf_delay_ms(10);
        }
    }
}

void pwm_init(void) {
    nrfx_pwm_config_t config;

    config.output_pins[0] = LED0_PIN;
    config.output_pins[1] = LED1_PIN;
    config.output_pins[2] = LED2_PIN;
    config.output_pins[3] = LED3_PIN;

    config.irq_priority = 7;

    config.base_clock = NRF_PWM_CLK_1MHz; 
    config.count_mode = NRF_PWM_MODE_UP;
    config.top_value = PWM_TOP_VALUE;
    config.load_mode = NRF_PWM_LOAD_INDIVIDUAL;
    config.step_mode = NRF_PWM_STEP_AUTO;

    nrfx_pwm_init(&m_pwm0, &config, NULL);
}

void button_init(void) {
    nrf_gpio_cfg_input(BUTTON_PIN, NRF_GPIO_PIN_PULLUP);
}

bool check_button_double_click(void) {
    static bool last_state = true;
    static uint32_t last_debounce_time = 0;
    static uint32_t first_click_time = 0;
    static bool first_click_detected = false;
    
    bool current_state = nrf_gpio_pin_read(BUTTON_PIN) == 0;
    uint32_t current_time = 0; // В реальном приложении здесь должно быть время
    
    // Простой дебаунсинг - используем задержку
    if (current_state != last_state) {
        last_debounce_time = current_time;
        last_state = current_state;
    }
    
    // Если кнопка нажата и состояние стабильно
    if (current_state && (current_time - last_debounce_time) > DEBOUNCE_DELAY_MS) {
        if (!first_click_detected) {
            // Первое нажатие
            first_click_detected = true;
            first_click_time = current_time;
            return false;
        } else {
            // Второе нажатие - проверяем временной интервал
            if ((current_time - first_click_time) < DOUBLE_CLICK_MAX_MS) {
                first_click_detected = false;
                return true; // Двойное нажатие обнаружено
            } else {
                // Слишком большой интервал - начинаем заново
                first_click_time = current_time;
                return false;
            }
        }
    }
    
    // Сбрасываем состояние, если прошло слишком много времени
    if (first_click_detected && (current_time - first_click_time) > DOUBLE_CLICK_MAX_MS) {
        first_click_detected = false;
    }
    
    return false;
}

// Упрощенная версия без точного времени
bool check_button_double_click_simple(void) {
    static bool last_state = true;
    static bool first_click = false;
    static uint32_t click_counter = 0;
    
    bool current_state = nrf_gpio_pin_read(BUTTON_PIN) == 0;
    
    // Обнаружение фронта нажатия
    if (current_state && !last_state) {
        nrf_delay_ms(DEBOUNCE_DELAY_MS); // Дебаунсинг
        if (nrf_gpio_pin_read(BUTTON_PIN) == 0) { // Подтверждаем нажатие
            if (!first_click) {
                first_click = true;
                click_counter = 0;
            } else {
                // Второе нажатие в течение короткого времени
                if (click_counter < (DOUBLE_CLICK_MAX_MS / 10)) {
                    first_click = false;
                    return true;
                }
            }
        }
    }
    
    last_state = current_state;
    
    // Счетчик для определения времени между нажатиями
    if (first_click) {
        click_counter++;
        nrf_delay_ms(10);
        if (click_counter >= (DOUBLE_CLICK_MAX_MS / 10)) {
            first_click = false; // Время вышло
        }
    }
    
    return false;
}

void set_pwm_duty(int led_index, uint16_t duty) {
    switch (led_index) {
        case 0: seq_values.channel_0 = duty; break;
        case 1: seq_values.channel_1 = duty; break;
        case 2: seq_values.channel_2 = duty; break;
        case 3: seq_values.channel_3 = duty; break;
    }

    nrfx_pwm_simple_playback(&m_pwm0, &pwm_seq, 1, NRFX_PWM_FLAG_LOOP);
}

void fade_in(int led_index, int time_ms) {
    int delay = time_ms / FADE_STEPS;

    for (int i = 0; i <= FADE_STEPS; i++) {
        if (!running) return; // Проверяем, не остановлен ли цикл
        uint16_t duty = (PWM_TOP_VALUE * i) / FADE_STEPS;
        set_pwm_duty(led_index, duty);
        nrf_delay_ms(delay);
    }
}

void fade_out(int led_index, int time_ms) {
    int delay = time_ms / FADE_STEPS;

    for (int i = FADE_STEPS; i >= 0; i--) {
        if (!running) return; // Проверяем, не остановлен ли цикл
        uint16_t duty = (PWM_TOP_VALUE * i) / FADE_STEPS;
        set_pwm_duty(led_index, duty);
        nrf_delay_ms(delay);
    }
}

void blink(int led_index) {
    fade_in(led_index, 400);
    fade_out(led_index, 400);
}