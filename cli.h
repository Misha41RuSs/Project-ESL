#ifndef USB_CLI_H
#define USB_CLI_H

#include <stdbool.h>
#include <stdint.h>


void usb_cli_init(void);
void usb_cli_process(void);

void set_rgb_color(uint8_t r, uint8_t g, uint8_t b);
void set_hsv_color(uint16_t h, uint8_t s, uint8_t v);
void save_settings(void); // Пока не работает
void load_settings(void); // Пока не работает
void get_status(uint16_t *h, uint8_t *s, uint8_t *v, uint8_t *r, uint8_t *g, uint8_t *b);


#endif