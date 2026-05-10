#ifndef SHELL_H
#define SHELL_H
#include <stdint.h>

void shell_init();
void shell_update();
void execute_command(char* input);
void dummy_app();
void run_top();
void shell_compile(const char* filename);
void update_shell_cwd(char* target);

void shell_print(char* str, uint32_t color);
void shell_draw_char(char c, int x, int y, uint32_t fg, uint32_t bg);
#endif
