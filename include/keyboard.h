#ifndef KOSOS_KEYBOARD_H
#define KOSOS_KEYBOARD_H

#include <stdint.h>

#define KEY_NONE 0
#define KEY_UP 1
#define KEY_DOWN 2
#define KEY_LEFT 3
#define KEY_RIGHT 4

typedef struct {
	char ch;
	uint8_t code;
	uint8_t ctrl;
} key_event_t;

void keyboard_init(void);
int keyboard_get_event(key_event_t *event);
char keyboard_getch(void);

#endif
