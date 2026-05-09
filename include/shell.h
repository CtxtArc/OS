#ifndef SHELL_H
#define SHELL_H


void shell_init();
void shell_update();
void execute_command(char* input);
void dummy_app();
void run_top();
void shell_compile(const char* filename);
void update_shell_cwd(char* target);
#endif
