#ifndef OLED_H
#define OLED_H

#include <stdint.h>

/*
 * Reconstructed oled.h  (SSD1306 128x64 I2C OLED support).
 * The upstream sbitx commit that added OLED support committed oled.c and
 * added #include "oled.h" to oled.c and sbitx_gtk.c, but never committed
 * this header -- so `./build sbitx` fails with "oled.h: No such file".
 * These are the standard SSD1306 control bytes + init sequence. On a radio
 * with no OLED fitted, oled_init() simply prints "oled display not detected".
 */

/* SSD1306 I2C address and control (Co/D-C) bytes */
#define OLED_ADDR     0x3C
#define OLED_COMMAND  0x00
#define OLED_DATA     0x40

/* SSD1306 128x64 power-on / init command sequence */
static const uint8_t oled_init_sequence[] __attribute__((unused)) = {
	0xAE,        /* display off                         */
	0xD5, 0x80,  /* clock divide ratio / osc frequency  */
	0xA8, 0x3F,  /* multiplex ratio = 1/64              */
	0xD3, 0x00,  /* display offset = 0                  */
	0x40,        /* start line = 0                      */
	0x8D, 0x14,  /* charge pump enable                  */
	0x20, 0x00,  /* memory addressing mode = horizontal */
	0xA1,        /* segment re-map (col 127 -> SEG0)    */
	0xC8,        /* COM scan direction remapped         */
	0xDA, 0x12,  /* COM pins hardware config            */
	0x81, 0x7F,  /* contrast                            */
	0xD9, 0xF1,  /* pre-charge period                   */
	0xDB, 0x40,  /* VCOMH deselect level                */
	0xA4,        /* resume to RAM content               */
	0xA6,        /* normal (non-inverted) display       */
	0xAF         /* display on                          */
};

/* implemented in oled.c */
void        oled_refresh();
int         oled_init();
const char *oled_write(uint8_t col, uint8_t row, const char *string);
void        oled_console(int style, char *string);
void        oled_clear();

#endif /* OLED_H */
