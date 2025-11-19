#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include "nrf_gpio.h"
#include "nrfx_pwm.h"
#include "nrfx_gpiote.h"
#include "app_timer.h"
#include "nrfx_clock.h"

#define LED0_PIN 6    // Индикатор (channel 0)
#define LED1_PIN 8    // R (channel 1)
#define LED2_PIN 41   // G (channel 2)
#define LED3_PIN 12   // B (channel 3)
#define BUTTON_PIN 38

#define PWM_CHANNELS       4
#define PWM_TOP_VALUE      1000U

// Основные интервалы
#define MAIN_INTERVAL_MS   20   // основной таймер — обновление PWM/индикатора/изменения при удержании
#define DEBOUNCE_MS        200
#define DOUBLE_CLICK_MS    600

// Поведение при удержании
#define HOLD_INTERVAL_MS   MAIN_INTERVAL_MS
#define HOLD_STEP_H        2    // градусы за шаг для H (0..360)
#define HOLD_STEP_SV       1    // проценты за шаг для S и V (0..100)

// Индикатор мигания (цикл плавного нарастания/спада)
#define SLOW_BLINK_PERIOD_MS   1500  // полный цикл (возрастание+убывание)
#define FAST_BLINK_PERIOD_MS    500

// Прототипы функций
void pwm_init(void);
void button_init(void);
void main_timer_handler(void * p_context);
void debounce_timer_handler(void * p_context);
void double_click_timer_handler(void * p_context);
void button_handler(nrfx_gpiote_pin_t pin, nrf_gpiote_polarity_t action);

// Глобальные переменные
static nrfx_pwm_t m_pwm_instance = NRFX_PWM_INSTANCE(0);
static nrf_pwm_values_individual_t m_seq_values;

typedef enum {
    MODE_NONE = 0,
    MODE_HUE,
    MODE_SAT,
    MODE_VAL
} input_mode_t;

static volatile input_mode_t m_mode = MODE_NONE;

// HSV state
static float m_h = 0.0f;    // 0..360
static int   m_s = 100;     // 0..100
static int   m_v = 100;     // 0..100

// Направления изменения при удержании (1 = увеличиваем, -1 = уменьшаем)
static int dir_h = 1;
static int dir_s = 1;
static int dir_v = 1;

// Индикатор (канал 0) PWM-уровень и направление для пульсации
static int m_indicator_duty = 0;
static int m_indicator_dir = 1; // направление для плавного мигания +1 / -1

// Флаги для кнопки/удержания
static volatile bool m_button_blocked = false;
static volatile bool m_first_click_detected = false;
static volatile bool m_button_held = false;

// Таймеры
APP_TIMER_DEF(main_timer);
APP_TIMER_DEF(debounce_timer);
APP_TIMER_DEF(double_click_timer);

// Вспомогательные: вычисление шагов индикатора по режиму
static uint32_t m_indicator_step = 1;
static uint32_t m_indicator_period_ms = SLOW_BLINK_PERIOD_MS;

// --- Вспомогательные функции ---

static inline int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// HSV (h:0..360, s:0..100, v:0..100) -> RGB (0..PWM_TOP_VALUE)
static void hsv_to_rgb(float h, int s, int v, uint16_t *r, uint16_t *g, uint16_t *b) {
    float H = h;
    float S = s / 100.0f;
    float V = v / 100.0f;

    if (S <= 0.0f) {
        uint16_t val = (uint16_t)(V * PWM_TOP_VALUE + 0.5f);
        *r = *g = *b = val;
        return;
    }

    if (H >= 360.0f) H = 0.0f;
    float hf = H / 60.0f;
    int i = (int)floorf(hf);
    float f = hf - i;
    float p = V * (1.0f - S);
    float q = V * (1.0f - S * f);
    float t = V * (1.0f - S * (1.0f - f));

    float rf=0, gf=0, bf=0;
    switch (i) {
        case 0: rf = V; gf = t; bf = p; break;
        case 1: rf = q; gf = V; bf = p; break;
        case 2: rf = p; gf = V; bf = t; break;
        case 3: rf = p; gf = q; bf = V; break;
        case 4: rf = t; gf = p; bf = V; break;
        case 5:
        default: rf = V; gf = p; bf = q; break;
    }

    *r = (uint16_t)(clamp_int((int)roundf(rf * PWM_TOP_VALUE), 0, PWM_TOP_VALUE));
    *g = (uint16_t)(clamp_int((int)roundf(gf * PWM_TOP_VALUE), 0, PWM_TOP_VALUE));
    *b = (uint16_t)(clamp_int((int)roundf(bf * PWM_TOP_VALUE), 0, PWM_TOP_VALUE));
}

static void pwm_write_channels(uint16_t ch0, uint16_t ch1, uint16_t ch2, uint16_t ch3) {
    m_seq_values.channel_0 = ch0;
    m_seq_values.channel_1 = ch1;
    m_seq_values.channel_2 = ch2;
    m_seq_values.channel_3 = ch3;

    nrf_pwm_sequence_t seq = {
        .values.p_individual = &m_seq_values,
        .length = PWM_CHANNELS,
        .repeats = 0,
        .end_delay = 0
    };

    nrfx_pwm_simple_playback(&m_pwm_instance, &seq, 1, 0);
}

static void update_indicator_params_for_mode(void) {
    switch (m_mode) {
        case MODE_NONE:
            m_indicator_period_ms = 0;
            break;
        case MODE_HUE:
            m_indicator_period_ms = SLOW_BLINK_PERIOD_MS;
            break;
        case MODE_SAT:
            m_indicator_period_ms = FAST_BLINK_PERIOD_MS;
            break;
        case MODE_VAL:
            m_indicator_period_ms = 1; // всегда включён — handled separately
            break;
    }
    if (m_indicator_period_ms > 0)
        m_indicator_step = (int)ceilf((float)PWM_TOP_VALUE * ( (float)MAIN_INTERVAL_MS / (m_indicator_period_ms / 2.0f) )); // половина периода — нарастание
    else
        m_indicator_step = PWM_TOP_VALUE;
    if (m_indicator_step < 1) m_indicator_step = 1;
}

// --- Инициализации ---

void pwm_init(void) {
    nrfx_pwm_config_t config = NRFX_PWM_DEFAULT_CONFIG;
    config.output_pins[0] = LED0_PIN;   // индикатор
    config.output_pins[1] = LED1_PIN;   // R
    config.output_pins[2] = LED2_PIN;   // G
    config.output_pins[3] = LED3_PIN;   // B
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

    app_timer_create(&main_timer, APP_TIMER_MODE_REPEATED, main_timer_handler);
    app_timer_start(main_timer, APP_TIMER_TICKS(MAIN_INTERVAL_MS), NULL);
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

// --- Обработчики таймеров и кнопки ---

void debounce_timer_handler(void *p_context) {
    (void)p_context;
    m_button_blocked = false;
}

void double_click_timer_handler(void *p_context) {
    (void)p_context;
    m_first_click_detected = false;
}

void button_handler(nrfx_gpiote_pin_t pin, nrf_gpiote_polarity_t action) {
    (void)pin; (void)action;

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
        // cycle mode
        if (m_mode == MODE_NONE) m_mode = MODE_HUE;
        else if (m_mode == MODE_HUE) m_mode = MODE_SAT;
        else if (m_mode == MODE_SAT) m_mode = MODE_VAL;
        else m_mode = MODE_NONE;

        // при входе в режим рекомендуется установить направление в положительное (чтобы всегда начинать увеличивать)
        dir_h = 1; dir_s = 1; dir_v = 1;

        update_indicator_params_for_mode();
    }

    // Начинаем удержание (handler вызывается на нажатие)
    m_button_held = true;
}

void main_timer_handler(void *p_context) {
    (void)p_context;

    // Если ранее была пометка удержания, но кнопка реально отпущена — сбросим флаг удержания
    if (m_button_held) {
        // подтянутый пин: при отпускании читаем 1; при нажатии — 0 (у тебя pullup)
        if (nrf_gpio_pin_read(BUTTON_PIN) != 0) {
            m_button_held = false;
        }
    }

    // Если удержание активно и режим редактирования != NONE, изменяем параметр в осциллирующем режиме
    if (m_button_held && m_mode != MODE_NONE) {
        if (m_mode == MODE_HUE) {
            m_h += dir_h * HOLD_STEP_H;
            if (m_h >= 360.0f) {
                m_h = 360.0f;
                dir_h = -1;
            } else if (m_h <= 0.0f) {
                m_h = 0.0f;
                dir_h = 1;
            }
        }
        else if (m_mode == MODE_SAT) {
            m_s += dir_s * HOLD_STEP_SV;
            if (m_s >= 100) {
                m_s = 100;
                dir_s = -1;
            } else if (m_s <= 0) {
                m_s = 0;
                dir_s = 1;
            }
        }
        else if (m_mode == MODE_VAL) {
            m_v += dir_v * HOLD_STEP_SV;
            if (m_v >= 100) {
                m_v = 100;
                dir_v = -1;
            } else if (m_v <= 0) {
                m_v = 0;
                dir_v = 1;
            }
        }
    }

    // --- индикатор (канал 0) ---
    uint16_t ind = 0;
    if (m_mode == MODE_NONE) {
        ind = 0;
        m_indicator_duty = 0;
    } else if (m_mode == MODE_VAL) {
        // всегда включён
        ind = PWM_TOP_VALUE;
        m_indicator_duty = PWM_TOP_VALUE;
    } else {
        // пульс: меняем duty на m_indicator_step * m_indicator_dir
        if (m_indicator_period_ms > 0) {
            m_indicator_duty += (int)m_indicator_step * m_indicator_dir;
            if (m_indicator_duty >= (int)PWM_TOP_VALUE) {
                m_indicator_duty = PWM_TOP_VALUE;
                m_indicator_dir = -1;
            } else if (m_indicator_duty <= 0) {
                m_indicator_duty = 0;
                m_indicator_dir = 1;
            }
            ind = (uint16_t)clamp_int(m_indicator_duty, 0, PWM_TOP_VALUE);
        } else {
            ind = 0;
        }
    }

    // --- обновляем цвет (каналы 1..3) ---
    uint16_t r, g, b;
    hsv_to_rgb(m_h, m_s, m_v, &r, &g, &b);

    pwm_write_channels(ind, r, g, b);
}

// --- main ---

int main(void) {
    nrfx_clock_init(NULL);
    nrfx_clock_lfclk_start();
    while(!nrfx_clock_lfclk_is_running());

    app_timer_init();

    // DEVICE_ID = 6577 -> последние 2 цифры = 77 -> hue% = 77% -> h = 0.77*360
    m_s = 100;
    m_v = 100;
    m_h = (77.0f / 100.0f) * 360.0f; // ≈ 277.2

    update_indicator_params_for_mode();

    pwm_init();
    button_init();

    // Показать стартовый цвет сразу
    uint16_t r, g, b;
    hsv_to_rgb(m_h, m_s, m_v, &r, &g, &b);
    pwm_write_channels(0, r, g, b);

    while (1) {
        __WFE();
    }
}
