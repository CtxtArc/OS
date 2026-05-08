#ifndef TASK_H
#define TASK_H
#include <stdint.h>

#define MAX_TASKS 10
#define STACK_SIZE 4096
#define TASK_KBD_BUF_SIZE 64
struct task {
    uint32_t esp;
    uint32_t state; // 0 = empty, 1 = ready, 2 = sleep, 3 = blocked 
    uint32_t sleep_ticks;
    char name[16];
    uint32_t vga_index; // Store the character position (0-3999)
    int last_x; // Track the X coordinate used in syscall
    int last_y; // Track the Y coordinate used in syscall
    int first_x; // Track the Y coordinate used in syscall
    int first_y; // Track the Y coordinate used in syscall
    int has_drawn; // Boolean flag: did this task ever print?
    void* stack_ptr; // Store this so we can kfree it!
    void* code_ptr;  // Store this so we can kfree it!
    uint32_t total_ticks; // Accumulated CPU time
    // --- Private Keyboard Mailbox ---
    char kbd_buffer[TASK_KBD_BUF_SIZE];
    uint8_t kbd_head;
    uint8_t kbd_tail;
    
    // --- Window Manager Data ---
    int has_window;         // 1 if it has a GUI window, 0 if it's a background task
    uint32_t* window_buffer;// The private memory canvas
    int win_x;              // Where the window sits on the real screen (X)
    int win_y;              // Where the window sits on the real screen (Y)
    int win_w;              // Window Width
    int win_h;              // Window Height
    int cursor_x;
    int cursor_y;
};

void init_multitasking();
void idle_task_code();
void yield();
int get_current_task_id();
int spawn_task(void (*entry_point)(), void* code_ptr, char* name);
void kill_task(int id);
uint32_t task_get_esp(int id);
int task_is_ready(int id);
void shell_task();
char* task_get_name(int id);
int task_get_state(int id);
int task_get_sleep_ticks(int id);
int task_get_total_ticks(int id);
void task_timer();
void task_game();
void run_top();

void refresh_tiling_layout();
void task_create_window(int tid, int x, int y, int w, int h);
extern volatile struct task task_list[MAX_TASKS]; 
extern int vesa_dirty; // Useful for the shell to trigger refreshes

void run_startup_tests();

void compositor_task();
#endif
