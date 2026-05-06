#ifndef KOSOS_CONSOLE_H
#define KOSOS_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint64_t address;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint8_t bpp;
} framebuffer_info_t;

void console_init(void);
void console_init_framebuffer(const framebuffer_info_t *info);
void console_clear(void);
void console_set_color(uint8_t color);
void console_putc(char c);
void console_putc_at(size_t row, size_t column, char c);
void console_write(const char *text);
void console_writeln(const char *text);
void console_write_dec(int value);
void console_write_hex(uint32_t value);
void console_set_cursor(size_t row, size_t column);

#endif
