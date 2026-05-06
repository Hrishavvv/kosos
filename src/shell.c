#include "shell.h"

#include "console.h"
#include "disk.h"
#include "editor.h"
#include "fs.h"
#include "keyboard.h"
#include "os.h"
#include "usb_msc.h"

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static const char *skip_spaces(const char *text) {
    while (*text != '\0' && is_space(*text)) {
        ++text;
    }
    return text;
}

static int strings_equal(const char *left, const char *right) {
    while (*left != '\0' && *right != '\0') {
        if (*left != *right) {
            return 0;
        }
        ++left;
        ++right;
    }

    return *left == '\0' && *right == '\0';
}

static int prompt_yes_no(const char *message) {
    console_write(message);
    for (;;) {
        char c = keyboard_getch();
        if (c == 'y' || c == 'Y') {
            console_putc(c);
            console_putc('\n');
            return 1;
        }
        if (c == 'n' || c == 'N') {
            console_putc(c);
            console_putc('\n');
            return 0;
        }
    }
}

static int command_is(const char *command, const char *primary, const char *alias) {
    if (strings_equal(command, primary)) {
        return 1;
    }
    if (alias != NULL && strings_equal(command, alias)) {
        return 1;
    }
    return 0;
}

static const char *parse_int_token(const char *text, int *value) {
    int sign = 1;
    long result = 0;

    text = skip_spaces(text);
    if (*text == '+') {
        ++text;
    } else if (*text == '-') {
        sign = -1;
        ++text;
    }

    if (*text < '0' || *text > '9') {
        return NULL;
    }

    while (*text >= '0' && *text <= '9') {
        result = result * 10 + (*text - '0');
        ++text;
    }

    *value = (int) (result * sign);
    return text;
}

static void print_prompt(void) {
    console_write("kosos:");
    console_write(fs_get_cwd());
    console_write("> ");
}

static void print_help(void) {
    console_writeln("help - show this help");
    console_writeln("about - show system hardware details");
    console_writeln("peek/ls [path] - list directory contents");
    console_writeln("slide/cd [path] - change directory (empty: /)");
    console_writeln("spawn/mkdir <path> - create a directory");
    console_writeln("tag/touch <path> - create an empty file");
    console_writeln("spill/cat <path> - show file contents");
    console_writeln("where/pwd - show current directory");
    console_writeln("ink/write <path> - open editor");
    console_writeln("yeet/rm <path> - delete file");
    console_writeln("shred/rmdir <path> - delete directory (recursive)");
    console_writeln("");
    console_writeln("clear - clear the screen");
    console_writeln("echo <text> - print text back");
    console_writeln("");
    console_writeln("add <a> <b> - add two numbers");
    console_writeln("sub <a> <b> - subtract two numbers");
    console_writeln("mul <a> <b> - multiply two numbers");
    console_writeln("div <a> <b> - divide two numbers");
    console_writeln("calc <a> <op> <b> - calculator with + - * /");
}

static void print_result(int value) {
    console_write("= ");
    console_write_dec(value);
    console_putc('\n');
}

static void run_binary_op(const char *args, char op, const char *name) {
    const char *cursor = args;
    int lhs;
    int rhs;

    cursor = parse_int_token(cursor, &lhs);
    if (cursor == NULL) {
        console_write("Usage: ");
        console_write(name);
        console_writeln(" <a> <b>");
        return;
    }

    cursor = parse_int_token(cursor, &rhs);
    if (cursor == NULL) {
        console_write("Usage: ");
        console_write(name);
        console_writeln(" <a> <b>");
        return;
    }

    cursor = skip_spaces(cursor);
    if (*cursor != '\0') {
        console_writeln("Too many arguments.");
        return;
    }

    if (op == '+') {
        print_result(lhs + rhs);
    } else if (op == '-') {
        print_result(lhs - rhs);
    } else if (op == '*') {
        print_result(lhs * rhs);
    } else if (op == '/') {
        if (rhs == 0) {
            console_writeln("Division by zero.");
        } else {
            print_result(lhs / rhs);
        }
    }
}

static void run_calc(const char *args) {
    const char *cursor = args;
    int lhs;
    int rhs;
    char op;

    cursor = parse_int_token(cursor, &lhs);
    if (cursor == NULL) {
        console_writeln("Usage: calc <a> <op> <b>");
        return;
    }

    cursor = skip_spaces(cursor);
    if (*cursor == '\0') {
        console_writeln("Usage: calc <a> <op> <b>");
        return;
    }

    op = *cursor;
    ++cursor;
    cursor = parse_int_token(cursor, &rhs);
    if (cursor == NULL) {
        console_writeln("Usage: calc <a> <op> <b>");
        return;
    }

    cursor = skip_spaces(cursor);
    if (*cursor != '\0') {
        console_writeln("Too many arguments.");
        return;
    }

    switch (op) {
        case '+':
            print_result(lhs + rhs);
            break;
        case '-':
            print_result(lhs - rhs);
            break;
        case '*':
            print_result(lhs * rhs);
            break;
        case '/':
            if (rhs == 0) {
                console_writeln("Division by zero.");
            } else {
                print_result(lhs / rhs);
            }
            break;
        default:
            console_writeln("Unknown operator.");
            break;
    }
}

static void handle_command(char *line) {
    char *cursor = line;

    cursor = (char *) skip_spaces(cursor);
    if (*cursor == '\0') {
        return;
    }

    char *command = cursor;
    while (*cursor != '\0' && !is_space(*cursor)) {
        ++cursor;
    }

    if (*cursor != '\0') {
        *cursor++ = '\0';
    }
    cursor = (char *) skip_spaces(cursor);

    if (strings_equal(command, "help")) {
        print_help();
    } else if (strings_equal(command, "clear")) {
        console_clear();
    } else if (strings_equal(command, "echo")) {
        console_writeln(cursor);
    } else if (command_is(command, "peek", "ls")) {
        const char *args = (*cursor == '\0') ? NULL : cursor;
        fs_ls(args);
    } else if (command_is(command, "slide", "cd")) {
        if (*cursor == '\0') {
            if (fs_cd("/") == 0) {
                console_writeln(fs_get_cwd());
            }
        } else if (fs_cd(cursor) == 0) {
            console_writeln(fs_get_cwd());
        }
    } else if (command_is(command, "spawn", "mkdir")) {
        if (*cursor == '\0') {
            console_writeln("Usage: spawn/mkdir <path>");
        } else {
            fs_mkdir(cursor);
        }
    } else if (command_is(command, "tag", "touch")) {
        if (*cursor == '\0') {
            console_writeln("Usage: tag/touch <path>");
        } else {
            fs_touch(cursor);
        }
    } else if (command_is(command, "spill", "cat")) {
        if (*cursor == '\0') {
            console_writeln("Usage: spill/cat <path>");
        } else {
            fs_cat(cursor);
        }
    } else if (command_is(command, "where", "pwd")) {
        console_writeln(fs_get_cwd());
    } else if (command_is(command, "ink", "write")) {
        if (*cursor == '\0') {
            console_writeln("Usage: ink/write <path>");
        } else {
            int type = 0;
            unsigned int size = 0;
            int exists = (fs_stat(cursor, &type, &size) == 0);
            if (exists && type != FS_ENTRY_FILE) {
                console_writeln("Not a file.");
            } else {
                if (exists && size > EDITOR_MAX_SIZE) {
                    console_writeln("File too large for editor.");
                    return;
                }
                if (exists) {
                    if (!prompt_yes_no("File exists. Edit and overwrite? (y/n): ")) {
                        return;
                    }
                }
                editor_run(cursor);
            }
        }
    } else if (command_is(command, "yeet", "rm")) {
        if (*cursor == '\0') {
            console_writeln("Usage: yeet/rm <path>");
        } else {
            fs_remove_file(cursor);
        }
    } else if (command_is(command, "shred", "rmdir")) {
        if (*cursor == '\0') {
            console_writeln("Usage: shred/rmdir <path>");
        } else {
            fs_remove_dir(cursor, 1);
        }
    } else if (strings_equal(command, "add")) {
        run_binary_op(cursor, '+', "add");
    } else if (strings_equal(command, "sub")) {
        run_binary_op(cursor, '-', "sub");
    } else if (strings_equal(command, "mul")) {
        run_binary_op(cursor, '*', "mul");
    } else if (strings_equal(command, "div")) {
        run_binary_op(cursor, '/', "div");
    } else if (strings_equal(command, "calc")) {
        run_calc(cursor);
    } else if (strings_equal(command, "about")) {
        uint32_t sectors = disk_total_sectors();
        uint32_t mb = sectors / 2048U;
        console_writeln("cpu: 32-bit x86 (protected mode)");
        console_writeln("video: VGA text 80x25");
        console_writeln("keyboard: PS/2 (polled)");
        
        const usb_msc_info_t *msc_info = usb_msc_get_info();
        if (usb_msc_ready() && msc_info->capacity_bytes > 0) {
            console_write("storage: USB MSC - ");
            console_write(msc_info->vendor);
            console_write(" ");
            console_write(msc_info->model);
            console_putc('\n');
            uint32_t gb = (msc_info->capacity_bytes >> 30);
            console_write("         ");
            console_write_dec(gb);
            console_writeln(" GB");
        } else if (sectors == 0) {
            console_writeln("storage: not detected");
        } else {
            console_write("storage: IDE primary master, ");
            console_write_dec((int) mb);
            console_writeln(" MB");
        }
        
        if (fs_is_ready()) {
            console_write("filesystem: ready, clusters ");
            console_write_dec((int) fs_cluster_count());
            console_putc('\n');
        } else {
            console_writeln("filesystem: offline");
        }
    } else {
        console_writeln("Unknown command. Type help.");
    }
}

void shell_run(void) {
    char line[128];
    size_t length = 0;

    console_write("Welcome to ");
    console_write(OS_NAME);
    console_writeln(".");
    console_writeln("Type help for commands.");

    for (;;) {
        print_prompt();
        length = 0;

        for (;;) {
            char c = keyboard_getch();

            if (c == '\n') {
                console_putc('\n');
                line[length] = '\0';
                handle_command(line);
                break;
            }

            if (c == '\b') {
                if (length > 0) {
                    --length;
                    console_putc('\b');
                }
                continue;
            }

            if (length + 1 < sizeof(line)) {
                line[length++] = c;
                console_putc(c);
            }
        }
    }
}
