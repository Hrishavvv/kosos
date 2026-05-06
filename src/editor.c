#include "editor.h"

#include "console.h"
#include "fs.h"
#include "keyboard.h"

#include <stdint.h>

#define EDITOR_COLS 80
#define EDITOR_ROWS 24
#define EDITOR_MAX_LINES 16384

static unsigned int g_line_starts[EDITOR_MAX_LINES];

struct editor_state {
    char buffer[EDITOR_MAX_SIZE + 1];
    unsigned int length;
    unsigned int cursor;
    unsigned int scroll_line;
    int dirty;
};

static unsigned int build_lines(const char *buffer, unsigned int length) {
    unsigned int count = 1;
    g_line_starts[0] = 0;

    for (unsigned int i = 0; i < length; ++i) {
        if (buffer[i] == '\n') {
            if (count < EDITOR_MAX_LINES) {
                g_line_starts[count++] = i + 1;
            }
        }
    }

    return count;
}

static unsigned int find_line(unsigned int line_count, unsigned int cursor) {
    unsigned int line = 0;
    for (unsigned int i = 1; i < line_count; ++i) {
        if (g_line_starts[i] > cursor) {
            break;
        }
        line = i;
    }
    return line;
}

static unsigned int line_end(unsigned int line, unsigned int line_count, unsigned int length) {
    if (line + 1 < line_count) {
        unsigned int end = g_line_starts[line + 1];
        if (end > 0) {
            return end - 1;
        }
        return 0;
    }
    return length;
}

static void draw_text(unsigned int row, unsigned int col, const char *text) {
    unsigned int x = col;
    for (unsigned int i = 0; text[i] != '\0' && x < EDITOR_COLS; ++i, ++x) {
        console_putc_at(row, x, text[i]);
    }
}

static unsigned int text_len(const char *text) {
    unsigned int len = 0;
    while (text[len] != '\0') {
        ++len;
    }
    return len;
}

static void clear_row(unsigned int row) {
    for (unsigned int col = 0; col < EDITOR_COLS; ++col) {
        console_putc_at(row, col, ' ');
    }
}

static void editor_render(struct editor_state *state, const char *path) {
    unsigned int line_count = build_lines(state->buffer, state->length);
    unsigned int cursor_line = find_line(line_count, state->cursor);

    if (cursor_line < state->scroll_line) {
        state->scroll_line = cursor_line;
    } else if (cursor_line >= state->scroll_line + EDITOR_ROWS) {
        state->scroll_line = cursor_line - (EDITOR_ROWS - 1);
    }

    console_clear();
    clear_row(0);
    draw_text(0, 0, "EDIT ");
    draw_text(0, 5, path);
    unsigned int path_len = text_len(path);
    unsigned int hint_col = 5 + path_len;
    if (hint_col + 1 < EDITOR_COLS) {
        draw_text(0, hint_col, " | Ctrl+S save | Ctrl+Q quit");
    }

    for (unsigned int row = 0; row < EDITOR_ROWS; ++row) {
        unsigned int line_index = state->scroll_line + row;
        clear_row(row + 1);
        if (line_index >= line_count) {
            continue;
        }

        unsigned int start = g_line_starts[line_index];
        unsigned int end = line_end(line_index, line_count, state->length);
        unsigned int col = 0;

        for (unsigned int i = start; i < end && col < EDITOR_COLS; ++i, ++col) {
            char ch = state->buffer[i];
            if (ch == '\t') {
                ch = ' ';
            }
            console_putc_at(row + 1, col, ch);
        }
    }

    unsigned int cursor_col = state->cursor - g_line_starts[cursor_line];
    if (cursor_col >= EDITOR_COLS) {
        cursor_col = EDITOR_COLS - 1;
    }
    unsigned int cursor_row = cursor_line - state->scroll_line;
    if (cursor_row >= EDITOR_ROWS) {
        cursor_row = EDITOR_ROWS - 1;
    }

    console_set_cursor(cursor_row + 1, cursor_col);
}

static void editor_insert(struct editor_state *state, char ch) {
    if (state->length >= EDITOR_MAX_SIZE) {
        return;
    }

    for (unsigned int i = state->length; i > state->cursor; --i) {
        state->buffer[i] = state->buffer[i - 1];
    }

    state->buffer[state->cursor] = ch;
    state->length += 1;
    state->cursor += 1;
    state->buffer[state->length] = '\0';
    state->dirty = 1;
}

static void editor_backspace(struct editor_state *state) {
    if (state->cursor == 0 || state->length == 0) {
        return;
    }

    for (unsigned int i = state->cursor - 1; i + 1 < state->length; ++i) {
        state->buffer[i] = state->buffer[i + 1];
    }

    state->cursor -= 1;
    state->length -= 1;
    state->buffer[state->length] = '\0';
    state->dirty = 1;
}

static void editor_move_left(struct editor_state *state) {
    if (state->cursor > 0) {
        state->cursor -= 1;
    }
}

static void editor_move_right(struct editor_state *state) {
    if (state->cursor < state->length) {
        state->cursor += 1;
    }
}

static void editor_move_vertical(struct editor_state *state, int direction) {
    unsigned int line_count = build_lines(state->buffer, state->length);
    unsigned int cursor_line = find_line(line_count, state->cursor);
    unsigned int col = state->cursor - g_line_starts[cursor_line];

    if (direction < 0) {
        if (cursor_line == 0) {
            return;
        }
        cursor_line -= 1;
    } else {
        if (cursor_line + 1 >= line_count) {
            return;
        }
        cursor_line += 1;
    }

    unsigned int end = line_end(cursor_line, line_count, state->length);
    unsigned int start = g_line_starts[cursor_line];
    unsigned int line_len = (end > start) ? (end - start) : 0;

    if (col > line_len) {
        col = line_len;
    }

    state->cursor = start + col;
}

static void editor_save(struct editor_state *state, const char *path) {
    if (fs_write_file(path, (const uint8_t *) state->buffer, state->length, 1) == 0) {
        state->dirty = 0;
    } else {
        console_writeln("Save failed.");
    }
}

void editor_run(const char *path) {
    struct editor_state state;
    state.length = 0;
    state.cursor = 0;
    state.scroll_line = 0;
    state.dirty = 0;
    state.buffer[0] = '\0';

    if (path == NULL || *path == '\0') {
        console_writeln("Usage: ink/write <path>");
        return;
    }

    unsigned int size = 0;
    if (fs_read_file(path, (uint8_t *) state.buffer, EDITOR_MAX_SIZE, &size) == 0) {
        state.length = size;
        state.cursor = size;
        state.buffer[state.length] = '\0';
    }

    for (;;) {
        editor_render(&state, path);

        key_event_t event;
        if (!keyboard_get_event(&event)) {
            continue;
        }

        if (event.ctrl) {
            char key = event.ch;
            if (key >= 'A' && key <= 'Z') {
                key = (char) (key - 'A' + 'a');
            }
            if (key == 's') {
                editor_save(&state, path);
                continue;
            }
            if (key == 'q') {
                console_clear();
                return;
            }
        }

        if (event.code == KEY_LEFT) {
            editor_move_left(&state);
            continue;
        }
        if (event.code == KEY_RIGHT) {
            editor_move_right(&state);
            continue;
        }
        if (event.code == KEY_UP) {
            editor_move_vertical(&state, -1);
            continue;
        }
        if (event.code == KEY_DOWN) {
            editor_move_vertical(&state, 1);
            continue;
        }

        if (event.ch == '\b') {
            editor_backspace(&state);
            continue;
        }

        if (event.ch == '\n') {
            editor_insert(&state, '\n');
            continue;
        }

        if (event.ch >= 32 && event.ch <= 126) {
            editor_insert(&state, event.ch);
        }
    }
}
