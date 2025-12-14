#include "cli.h"

#if ESTC_USB_CLI_ENABLED

#include "nrf_cli.h"
#include "nrf_cli_cdc_acm.h"
#include "nrf_log.h"
#include "app_usbd.h"
#include "app_usbd_core.h"
#include "app_usbd_serial_num.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "nrfx_nvmc.h"

NRF_CLI_CDC_ACM_DEF(m_cli_cdc_acm_transport);

NRF_CLI_DEF(m_cli_cdc_acm,
            "ledcli> ",
            &m_cli_cdc_acm_transport.transport,
            '\r', 
            4);

extern volatile float m_h;
extern volatile int m_s;
extern volatile int m_v;

#define COLORS_FLASH_ADDR 0x7E000
#define COLORS_MAGIC 0xC0A0BEEF

static color_entry_t m_colors[MAX_COLORS];

typedef struct {
    uint32_t magic;
    color_entry_t colors[MAX_COLORS];
} flash_colors_t;

static void hsv_to_rgb_for_cli(float h, int s, int v,
                               uint8_t *r, uint8_t *g, uint8_t *b);

static int find_color_index(const char *name) {
    for (int i = 0; i < MAX_COLORS; i++) {
        if (m_colors[i].used &&
            strncmp(m_colors[i].name, name, COLOR_NAME_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_free_slot(void) {
    for (int i = 0; i < MAX_COLORS; i++) {
        if (!m_colors[i].used)
            return i;
    }
    return -1;
}

static void cmd_add_rgb(nrf_cli_t const *p_cli, size_t argc, char **argv) {
    if (argc != 5) {
        nrf_cli_fprintf(p_cli, NRF_CLI_ERROR,
            "Использование: add_rgb_color <r> <g> <b> <name>\n");
        return;
    }

    int r = atoi(argv[1]);
    int g = atoi(argv[2]);
    int b = atoi(argv[3]);

    if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
        nrf_cli_fprintf(p_cli, NRF_CLI_ERROR,
            "Значения RGB должны быть в диапазоне 0-255\n");
        return;
    }

    if (find_color_index(argv[4]) >= 0) {
        nrf_cli_fprintf(p_cli, NRF_CLI_ERROR,
            "Такой цвет уже есть\n");
        return;
    }

    int slot = find_free_slot();
    if (slot < 0) {
        nrf_cli_fprintf(p_cli, NRF_CLI_ERROR,
            "Максимальное число цветов = %d\n", MAX_COLORS);
        return;
    }

    float rf = r / 255.0f;
    float gf = g / 255.0f;
    float bf = b / 255.0f;

    float max = fmaxf(rf, fmaxf(gf, bf));
    float min = fminf(rf, fminf(gf, bf));
    float delta = max - min;

    float h = 0.0f;
    if (delta > 0.0f) {
        if (max == rf)
            h = 60.0f * fmodf(((gf - bf) / delta), 6.0f);
        else if (max == gf)
            h = 60.0f * (((bf - rf) / delta) + 2.0f);
        else
            h = 60.0f * (((rf - gf) / delta) + 4.0f);

        if (h < 0.0f) h += 360.0f;
    }

    float s = (max == 0.0f) ? 0.0f : (delta / max);
    float v = max;

    m_colors[slot].h = (uint16_t)h;
    m_colors[slot].s = (uint8_t)(s * 100.0f);
    m_colors[slot].v = (uint8_t)(v * 100.0f);
    strncpy(m_colors[slot].name, argv[4], COLOR_NAME_LEN);
    m_colors[slot].used = true;

    nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL,
        "RGB: цвет '%s' добавлен\n", argv[4]);
    save_colors_to_flash();
}


static void cmd_add_hsv(nrf_cli_t const *p_cli, size_t argc, char **argv) {
    if (argc != 5) {
        nrf_cli_fprintf(p_cli, NRF_CLI_ERROR,
            "Использование: add_hsv_color <h> <s> <v> <name>\n");
        return;
    }

    if (find_color_index(argv[4]) >= 0) {
        nrf_cli_fprintf(p_cli, NRF_CLI_ERROR,
            "Такой цвет уже есть\n");
        return;
    }

    int slot = find_free_slot();
    if (slot < 0) {
        nrf_cli_fprintf(p_cli, NRF_CLI_ERROR,
            "Максимальное число цветов = %d\n", MAX_COLORS);
        return;
    }

    m_colors[slot].h = atoi(argv[1]);
    m_colors[slot].s = atoi(argv[2]);
    m_colors[slot].v = atoi(argv[3]);
    strncpy(m_colors[slot].name, argv[4], COLOR_NAME_LEN);
    m_colors[slot].used = true;

    nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL,
        "HSV: цвет '%s' добавлен\n", argv[4]);
    save_colors_to_flash();
}

static void cmd_add_current(nrf_cli_t const *p_cli, size_t argc, char **argv) {
    if (argc != 2) {
        nrf_cli_fprintf(p_cli, NRF_CLI_ERROR,
            "Использование: add_current_color <name>\n");
        return;
    }

    int slot = find_free_slot();
    if (slot < 0) {
        nrf_cli_fprintf(p_cli, NRF_CLI_ERROR,
            "Достигнута максимальная вместимость кол-ва цветов, удалите какой-нибудь\n");
        return;
    }

    m_colors[slot].h = (uint16_t)m_h;
    m_colors[slot].s = (uint8_t)m_s;
    m_colors[slot].v = (uint8_t)m_v;
    strncpy(m_colors[slot].name, argv[1], COLOR_NAME_LEN);
    m_colors[slot].used = true;

    nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL,
        "Текущий цвет сохранен под именем '%s'\n", argv[1]);
    save_colors_to_flash();
}

static void cmd_apply_color(nrf_cli_t const *p_cli, size_t argc, char **argv) {
    if (argc != 2) {
        nrf_cli_fprintf(p_cli, NRF_CLI_ERROR,
            "Использование: apply_color <name>\n");
        return;
    }

    int idx = find_color_index(argv[1]);
    if (idx < 0) {
        nrf_cli_fprintf(p_cli, NRF_CLI_ERROR,
            "Цвет не найден\n");
        return;
    }

    m_h = m_colors[idx].h;
    m_s = m_colors[idx].s;
    m_v = m_colors[idx].v;

    nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL,
        "Цвет с именем '%s' применён!\n", argv[1]);
}

static void cmd_list_colors(nrf_cli_t const *p_cli, size_t argc, char **argv) {
    (void)argc; (void)argv;

    nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL, "Сохраненные цвета:\n");
    for (int i = 0; i < MAX_COLORS; i++) {
        if (m_colors[i].used) {
            uint8_t r, g, b;
            hsv_to_rgb_for_cli((float)m_colors[i].h,
                   m_colors[i].s,
                   m_colors[i].v,
                   &r, &g, &b);

            nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL,
                "  %s: HSV(H=%d S=%d V=%d) RGB(%d,%d,%d)\n",
                m_colors[i].name,
                m_colors[i].h,
                m_colors[i].s,
                m_colors[i].v,
                r, g, b);
        }
    }
}

static void cmd_del_color(nrf_cli_t const *p_cli, size_t argc, char **argv) {
    if (argc != 2) {
        nrf_cli_fprintf(p_cli, NRF_CLI_ERROR,
            "Использование: del_color <name>\n");
        return;
    }

    int idx = find_color_index(argv[1]);
    if (idx < 0) {
        nrf_cli_fprintf(p_cli, NRF_CLI_ERROR,
            "Цвет не найден\n");
        return;
    }

    m_colors[idx].used = false;
    save_colors_to_flash();
    nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL,
        "Цвет с именем '%s' удалён\n", argv[1]);
}


static void hsv_to_rgb_for_cli(float h, int s, int v, uint8_t *r, uint8_t *g, uint8_t *b) {
    float H = h;
    float S = s / 100.0f;
    float V = v / 100.0f;

    if (S <= 0.0f) {
        uint8_t val = (uint8_t)(V * 255);
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

    *r = (uint8_t)(rf * 255);
    *g = (uint8_t)(gf * 255);
    *b = (uint8_t)(bf * 255);
}

static void cmd_rgb(nrf_cli_t const * p_cli, size_t argc, char ** argv) {
    if (argc != 4)
    {
        nrf_cli_fprintf(p_cli, NRF_CLI_ERROR, "Команда должна быть в формате: RGB <r> <g> <b>\n");
        return;
    }

    uint32_t r_in = atoi(argv[1]);
    uint32_t g_in = atoi(argv[2]);
    uint32_t b_in = atoi(argv[3]);

    if (r_in > 255 || g_in > 255 || b_in > 255)
    {
        nrf_cli_fprintf(p_cli, NRF_CLI_ERROR, "Ошибка: Значения должны быть в диапазоне 0-255\n");
        return;
    }

    float r_norm = r_in / 255.0f;
    float g_norm = g_in / 255.0f;
    float b_norm = b_in / 255.0f;
    
    float max = r_norm;
    if (g_norm > max) max = g_norm;
    if (b_norm > max) max = b_norm;
    
    float min = r_norm;
    if (g_norm < min) min = g_norm;
    if (b_norm < min) min = b_norm;
    
    float delta = max - min;
    
    float h = 0;
    if (delta != 0) {
        if (max == r_norm) {
            h = 60 * fmodf(((g_norm - b_norm) / delta), 6);
        } else if (max == g_norm) {
            h = 60 * (((b_norm - r_norm) / delta) + 2);
        } else {
            h = 60 * (((r_norm - g_norm) / delta) + 4);
        }
        
        if (h < 0) h += 360;
    }
    
    float s = (max == 0) ? 0 : (delta / max);
    
    m_h = h;
    m_s = (int)(s * 100);
    m_v = (int)(max * 100);
    
    nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL, "Цвет установлен: R=%d G=%d B=%d (HSV: H=%d S=%d V=%d)\n", 
                    r_in, g_in, b_in, (int)m_h, m_s, m_v);
}

static void cmd_hsv(nrf_cli_t const * p_cli, size_t argc, char ** argv)
{
    if (argc != 4)
    {
        nrf_cli_fprintf(p_cli, NRF_CLI_ERROR, "Команда должна быть в формате: HSV <h> <s> <v>\n");
        nrf_cli_fprintf(p_cli, NRF_CLI_ERROR, "H: 0-360, S: 0-100, V: 0-100\n");
        return;
    }
    
    uint16_t h = atoi(argv[1]);
    uint8_t s = atoi(argv[2]);
    uint8_t v = atoi(argv[3]);
    
    if (h > 360 || s > 100 || v > 100) {
        nrf_cli_fprintf(p_cli, NRF_CLI_ERROR, "Ошибка: H должен быть 0-360, S и V 0-100\n");
        return;
    }
    
    m_h = (float)h;
    m_s = s;
    m_v = v;
    
    nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL, "Цвет установлен в H=%d S=%d V=%d\n", h, s, v);
}

static void cmd_status(nrf_cli_t const * p_cli, size_t argc, char ** argv) {
    (void)argc;
    (void)argv;
    
    uint8_t r, g, b;
    hsv_to_rgb_for_cli(m_h, m_s, m_v, &r, &g, &b);
    
    nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL, "Текущие параметры цвета:\n");
    nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL, "  HSV: H=%d, S=%d%%, V=%d%%\n", (int)m_h, m_s, m_v);
    nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL, "  RGB: R=%d, G=%d, B=%d\n", r, g, b);
}

static void cmd_save(nrf_cli_t const * p_cli, size_t argc, char ** argv) {
    (void)argc;
    (void)argv;
    
    save_settings();
    nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL, "Настройки сохранены в память\n");
}

static void cmd_load(nrf_cli_t const * p_cli, size_t argc, char ** argv) {
    (void)argc;
    (void)argv;
    
    load_settings();
    nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL, "Настройки загружены из памяти\n");
}

static void cmd_reset(nrf_cli_t const * p_cli, size_t argc, char ** argv) {
    (void)argc;
    (void)argv;
    
    m_h = (77.0f / 100.0f) * 360.0f; 
    m_s = 100;
    m_v = 100;
    
    nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL, "Настройки сброшены по варианту #6577: H=%d, S=%d, V=%d\n", (int)m_h, m_s, m_v);
}

static void cmd_help(nrf_cli_t const * p_cli, size_t argc, char ** argv) {
    (void)argc;
    (void)argv;
    
    nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL, "Список команд:\n");
    nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL, "  RGB <r> <g> <b>   - Устанавливает цвет согласно цветовой модели RGB (0-255)\n");
    nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL, "  HSV <h> <s> <v>   - Устанавливает цвет согласно цветовой модели HSV (H:0-360, S:0-100, V:0-100)\n");
    nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL, "  STATUS            - Показывает текущий статус цвета\n");
    nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL, "  RESET             - Сбрасывает цвет согласно варианту #6577\n");
    nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL, "  HELP              - Показывает информацию о доступных командах\n");
    nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL,
        "  add_rgb_color <r> <g> <b> <name>   - Добавляет RGB цвет в память (0-255)\n");
    nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL,
        "  add_hsv_color <h> <s> <v> <name>   - Добавляет HSV цвет в память (H:0-360, S/V:0-100)\n");
    nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL,
        "  add_current_color <name>           - Сохраняет текущий цвет\n");
    nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL,
        "  del_color <name>                   - Удаляет цвет\n");
    nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL,
        "  apply_color <name>                 - Применяет выбранный цвет\n");
    nrf_cli_fprintf(p_cli, NRF_CLI_NORMAL,
        "  list_colors                        - Список сохранённых цветов\n");
}

NRF_CLI_CMD_REGISTER(RGB, NULL, "Set RGB color", cmd_rgb);
NRF_CLI_CMD_REGISTER(HSV, NULL, "Set HSV color", cmd_hsv);
NRF_CLI_CMD_REGISTER(STATUS, NULL, "Show current status", cmd_status);
NRF_CLI_CMD_REGISTER(RESET, NULL, "Reset to default color", cmd_reset);
NRF_CLI_CMD_REGISTER(HELP, NULL, "Show help", cmd_help);
NRF_CLI_CMD_REGISTER(add_hsv_color, NULL, "Add HSV color", cmd_add_hsv);
NRF_CLI_CMD_REGISTER(add_current_color, NULL, "Save current color", cmd_add_current);
NRF_CLI_CMD_REGISTER(apply_color, NULL, "Apply saved color", cmd_apply_color);
NRF_CLI_CMD_REGISTER(list_colors, NULL, "List saved colors", cmd_list_colors);
NRF_CLI_CMD_REGISTER(del_color, NULL, "Delete color", cmd_del_color);
NRF_CLI_CMD_REGISTER(add_rgb_color, NULL, "Add RGB color", cmd_add_rgb);

static void usbd_user_ev_handler(app_usbd_event_type_t event) {
    switch (event)
    {
        case APP_USBD_EVT_STOPPED:
            app_usbd_disable();
            break;
        case APP_USBD_EVT_POWER_DETECTED:
            if (!nrf_drv_usbd_is_enabled())
            {
                app_usbd_enable();
            }
            break;
        case APP_USBD_EVT_POWER_REMOVED:
            app_usbd_stop();
            break;
        case APP_USBD_EVT_POWER_READY:
            app_usbd_start();
            break;
        default:
            break;
    }
}


void set_rgb_color(uint8_t r, uint8_t g, uint8_t b) {
    char *argv[] = {"RGB", "0", "0", "0"};
    
    char r_str[4], g_str[4], b_str[4];
    snprintf(r_str, sizeof(r_str), "%d", r);
    snprintf(g_str, sizeof(g_str), "%d", g);
    snprintf(b_str, sizeof(b_str), "%d", b);
    
    argv[1] = r_str;
    argv[2] = g_str;
    argv[3] = b_str;
    
    cmd_rgb(&m_cli_cdc_acm, 4, argv);
}

void set_hsv_color(uint16_t h, uint8_t s, uint8_t v) {
    char *argv[] = {"HSV", "0", "0", "0"};
    
    char h_str[6], s_str[4], v_str[4];
    snprintf(h_str, sizeof(h_str), "%d", h);
    snprintf(s_str, sizeof(s_str), "%d", s);
    snprintf(v_str, sizeof(v_str), "%d", v);
    
    argv[1] = h_str;
    argv[2] = s_str;
    argv[3] = v_str;
    
    cmd_hsv(&m_cli_cdc_acm, 4, argv);
}

void save_settings(void) {
    cmd_save(&m_cli_cdc_acm, 1, NULL);
}

void load_settings(void) {
    cmd_load(&m_cli_cdc_acm, 1, NULL);
}

void get_status(uint16_t *h, uint8_t *s, uint8_t *v, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (h) *h = (uint16_t)m_h;
    if (s) *s = (uint8_t)m_s;
    if (v) *v = (uint8_t)m_v;
    
    if (r && g && b) {
        hsv_to_rgb_for_cli(m_h, m_s, m_v, r, g, b);
    }
}

void usb_cli_init(void) {
    ret_code_t ret;

    ret = nrf_cli_init(&m_cli_cdc_acm, NULL, true, true, NRF_LOG_SEVERITY_INFO);
    APP_ERROR_CHECK(ret);

    static const app_usbd_config_t usbd_config = {
        .ev_state_proc = usbd_user_ev_handler
    };

    app_usbd_serial_num_generate();
    ret = app_usbd_init(&usbd_config);
    APP_ERROR_CHECK(ret);

    app_usbd_class_inst_t const * class_cdc_acm = app_usbd_cdc_acm_class_inst_get(&nrf_cli_cdc_acm);
    ret = app_usbd_class_append(class_cdc_acm);
    APP_ERROR_CHECK(ret);

    ret = app_usbd_power_events_enable();
    APP_ERROR_CHECK(ret);

    ret = nrf_cli_start(&m_cli_cdc_acm);
    APP_ERROR_CHECK(ret);
}

void usb_cli_process(void) {
    nrf_cli_process(&m_cli_cdc_acm);
}

void save_colors_to_flash(void) {
    flash_colors_t data;
    data.magic = COLORS_MAGIC;
    memcpy(data.colors, m_colors, sizeof(m_colors));

    nrfx_nvmc_page_erase(COLORS_FLASH_ADDR);
    nrfx_nvmc_words_write(COLORS_FLASH_ADDR,
        (uint32_t *)&data,
        sizeof(data) / 4);
}

void load_colors_from_flash(void) {
    flash_colors_t *p =
        (flash_colors_t *)COLORS_FLASH_ADDR;

    if (p->magic != COLORS_MAGIC)
        return;

    memcpy(m_colors, p->colors, sizeof(m_colors));
}


#else

void usb_cli_init(void) {}
void usb_cli_process(void) {}
void set_rgb_color(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }
void set_hsv_color(uint16_t h, uint8_t s, uint8_t v) { (void)h; (void)s; (void)v; }
void save_settings(void) {}
void load_settings(void) {}
void get_status(uint16_t *h, uint8_t *s, uint8_t *v, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (h) *h = 0;
    if (s) *s = 0;
    if (v) *v = 0;
    if (r) *r = 0;
    if (g) *g = 0;
    if (b) *b = 0;
}

#endif