#include "task.h"
#include "vesa.h"
#include <stdint.h>
#include "kheap.h"
#include "io.h"
#include "shell.h"
#include "lib.h"

extern uint32_t* VESA_get_back_buffer();
int keyboard_focus_tid = 0;
extern int vesa_updating;
extern void switch_to_stack(uint32_t* old_esp, uint32_t new_esp);
extern volatile uint32_t system_ticks;
int multitasking_enabled = 0;
volatile struct task task_list[MAX_TASKS];
int current_task_idx = 0;


void shell_task() {
    char line[128];
    int idx = 0;

    VESA_print("KDXOS Shell Started.\n", COLOR_CYAN);
    VESA_print("> ", COLOR_YELLOW);

    while(1) {
        if (task_list[0].cursor_x >= task_list[0].win_w) {
            task_list[0].cursor_x = 0;
            task_list[0].cursor_y += 10;
        }

        char c = keyboard_getchar();

        if (c == '\n') {
            line[idx] = '\0';
            VESA_print("\n", COLOR_WHITE);
            if (idx > 0) execute_command(line);
            idx = 0;
            VESA_print("> ", COLOR_YELLOW);
    task_list[0].has_drawn = 1;  // ensure compositor picks up the prompt too
        }
        else if (c == '\b' && idx > 0) {
            idx--;
            if (task_list[0].cursor_x >= 8) task_list[0].cursor_x -= 0x08;
            VESA_draw_char(' ', task_list[0].cursor_x, task_list[0].cursor_y, 0x222222);
            task_list[0].has_drawn = 1;
        }
        else if (idx < 127 && c >= ' ') {
            line[idx++] = c;
            char str[2] = {c, '\0'};
            VESA_print(str, COLOR_WHITE);
        }
    }
}

int spawn_task(void (*entry_point)(), void* code_ptr, char* name) {
    for (int i = 1; i < MAX_TASKS; i++) {
        if (task_list[i].state == 0) {
            kstrncpy((char*)task_list[i].name, name, 15);
            task_list[i].name[15] = '\0';
            task_list[i].total_ticks = 0;
            task_list[i].sleep_ticks = 0;
            task_list[i].has_drawn = 0;
            task_list[i].has_window = 0;
            task_list[i].window_buffer = NULL;

            void* raw_stack = kmalloc(4096 + 0x1000);
            if (!raw_stack) return -1;
            task_list[i].stack_ptr = raw_stack;
            task_list[i].code_ptr = code_ptr;

            uint32_t aligned_stack = (uint32_t)raw_stack;
            if (aligned_stack & 0xFFF) {
                aligned_stack &= 0xFFFFF000;
                aligned_stack += 0x1000;
            }

            uint32_t* s_ptr = (uint32_t*)(aligned_stack + 4096);
            *--s_ptr = 0x10;
            uint32_t task_stack_top = (uint32_t)s_ptr;
            *--s_ptr = task_stack_top;
            *--s_ptr = 0x202;
            *--s_ptr = 0x08;
            *--s_ptr = (uint32_t)entry_point;
            *--s_ptr = 0;
            *--s_ptr = 32;
            for(int j = 0; j < 8; j++) *--s_ptr = 0;
            *--s_ptr = 0x10;

            task_list[i].esp = (uint32_t)s_ptr;
            task_list[i].state = 1;
            return i;
        }
    }
    return -1;
}

uint32_t task_stacks[MAX_TASKS][1024];

void yield() {
    __asm__ volatile("hlt");
}

void kill_task(int id) {
    if (id <= 0 || id >= MAX_TASKS) return;

    if (task_list[id].has_window) {
        int border = 2;
        int title_bar = 15;
        int full_w = task_list[id].win_w + (border * 2);
        int full_h = task_list[id].win_h + title_bar + border;
        VESA_clear_region(task_list[id].win_x, task_list[id].win_y, full_w, full_h);
        vesa_updating = 1;
        VESA_flip();
        vesa_updating = 0;
        if (task_list[id].window_buffer) {
            kfree(task_list[id].window_buffer);
            task_list[id].window_buffer = NULL;
        }
    }

    task_list[id].state = 0;
    if (task_list[id].stack_ptr) { kfree(task_list[id].stack_ptr); task_list[id].stack_ptr = NULL; }
    if (task_list[id].code_ptr)  { kfree(task_list[id].code_ptr);  task_list[id].code_ptr  = NULL; }
    task_list[id].has_window = 0;
    task_list[id].has_drawn  = 0;
}

void idle_task_code() {
    while(1) __asm__ volatile("hlt");
}

void init_multitasking() {
    multitasking_enabled = 1;

    for (int i = 0; i < MAX_TASKS; i++) {
        task_list[i].state         = 0;
        task_list[i].has_window    = 0;
        task_list[i].window_buffer = NULL;
        task_list[i].kbd_head      = 0;
        task_list[i].kbd_tail      = 0;
        task_list[i].total_ticks   = 0;
        task_list[i].sleep_ticks   = 0;
    }

    task_list[0].state = 1;
    kstrncpy((char*)task_list[0].name, "shell", 15);
    keyboard_focus_tid = 0;
    task_create_window(0, 50, 50, 600, 400);

    spawn_task(idle_task_code, NULL, "idle");
    spawn_task(compositor_task, NULL, "wm");

    current_task_idx = 0;
    refresh_tiling_layout();
}

int get_current_task_id() { return current_task_idx; }

int task_is_ready(int id) {
    if (id < 0 || id >= MAX_TASKS) return 0;
    return task_list[id].state == 1;
}

uint32_t task_get_esp(int id) {
    if (id < 0 || id >= MAX_TASKS) return 0;
    return task_list[id].esp;
}

char* task_get_name(int id) {
    if (id < 0 || id >= MAX_TASKS) return "unused";
    return (char*)task_list[id].name;
}

int task_get_state(int id) {
    if (id < 0 || id >= MAX_TASKS) return -1;
    return task_list[id].state;
}

int task_get_sleep_ticks(int id) {
    if (id < 0 || id >= MAX_TASKS) return -1;
    return task_list[id].sleep_ticks;
}

int task_get_total_ticks(int id) {
    if (id < 0 || id >= MAX_TASKS) return -1;
    return task_list[id].total_ticks;
}

void task_timer() {
    uint32_t seconds = 0;
    while (1) {
        seconds++;
        char buf[20];
        kmemset(buf, 0, 20);
        kstrcpy(buf, "TIMER: ");
        itoa(seconds, buf + 7, 10);
        VESA_print_at(buf, 900, 10, 0x00FFFF);
        sleep(1000);
    }
}

void task_game() {
    int previous_focus = keyboard_focus_tid;
    keyboard_focus_tid = current_task_idx;

    while (has_key_in_buffer()) { get_key_from_buffer(); }

    vesa_updating = 1;
    VESA_clear();
    vesa_updating = 0;

    int x = 500, y = 500;
    int old_x = 500, old_y = 500;
    int exit = 0;

    VESA_draw_char('*', x, y, 0x00FFFF);
    VESA_flip();

    while (exit == 0) {
        if (has_key_in_buffer()) {
            char c = get_key_from_buffer();
            if (c == 'q' || c == 'Q') { exit = 1; break; }

            old_x = x; old_y = y;
            switch (c) {
                case 'w': y -= 8; break;
                case 's': y += 8; break;
                case 'a': x -= 8; break;
                case 'd': x += 8; break;
            }

            vesa_updating = 1;
            VESA_clear_region(old_x, old_y, 8, 8);
            VESA_draw_char('*', x, y, 0x00FFFF);
            task_list[current_task_idx].first_x = x;
            task_list[current_task_idx].first_y = y;
            task_list[current_task_idx].last_x  = x + 8;
            task_list[current_task_idx].last_y  = y + 8;
            vesa_updating = 0;
            VESA_flip();
        } else {
            yield();
        }
    }

    vesa_updating = 1;
    VESA_clear();
    vesa_updating = 0;
    keyboard_focus_tid = previous_focus;
    task_list[current_task_idx].cursor_x = 0;
    task_list[current_task_idx].cursor_y = 0;
}

void run_top() {
    int previous_focus = keyboard_focus_tid;
    keyboard_focus_tid = current_task_idx;

    while (has_key_in_buffer()) { get_key_from_buffer(); }
    VESA_clear();

    while (1) {
        vesa_updating = 1;
        VESA_clear_buffer_only();

        kprintf_unsync("KDXOS TOP - System Ticks: %d\n", system_ticks);
        kprintf_unsync("Press 'q' to return to Shell\n");
        kprintf_unsync("-------------------------------------------\n");
        kprintf_unsync("TID   NAME         STATE      CPU-TICKS\n");

        for (int i = 0; i < MAX_TASKS; i++) {
            if (task_get_state(i) != 0) {
                kprintf_unsync("%d     %s", i, task_get_name(i));
                int name_len = kstrlen(task_get_name(i));
                for (int j = 0; j < (13 - name_len); j++) kprintf_unsync(" ");
                if      (task_get_state(i) == 1) kprintf_unsync("READY      ");
                else if (task_get_state(i) == 2) kprintf_unsync("SLEEP      ");
                kprintf_unsync("%d\n", task_get_total_ticks(i));
            }
        }

        vesa_updating = 0;
        VESA_flip();

        if (has_key_in_buffer()) {
            char c = get_key_from_buffer();
            if (c == 'q' || c == 'Q') break;
        }
        sleep(500);
    }

    VESA_clear_buffer_only();
    keyboard_focus_tid = previous_focus;
    kprintf_unsync("Returned to Shell.\n");
    VESA_flip();
}

void refresh_tiling_layout() {
    struct multiboot_info* mbi = VESA_get_boot_info();
    uint32_t sw = mbi->framebuffer_width;
    uint32_t sh = mbi->framebuffer_height;

    int gui_tasks = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_list[i].state != 0 && task_list[i].has_window) gui_tasks++;
    }

    if (gui_tasks > 0) {
        int tile_width = sw / gui_tasks;
        int current_tile = 0;
        for (int i = 0; i < MAX_TASKS; i++) {
            if (task_list[i].state != 0 && task_list[i].has_window) {
                task_list[i].win_x = current_tile * tile_width;
                task_list[i].win_w = tile_width;
                task_list[i].win_h = sh;
                current_tile++;
            }
        }
    }
}

void task_create_window(int tid, int x, int y, int w, int h) {
    if (tid < 0 || tid >= MAX_TASKS) return;
    struct multiboot_info* mbi = VESA_get_boot_info();
    uint32_t full_size = mbi->framebuffer_width * mbi->framebuffer_height;

    task_list[tid].has_window  = 1;
    task_list[tid].win_x       = x;
    task_list[tid].win_y       = y;
    task_list[tid].win_w       = w;
    task_list[tid].win_h       = h;
    task_list[tid].cursor_x    = 0;
    task_list[tid].cursor_y    = 0;

    task_list[tid].window_buffer = (uint32_t*)kmalloc(full_size * 4);
    if (task_list[tid].window_buffer) {
        for (uint32_t p = 0; p < full_size; p++)
            task_list[tid].window_buffer[p] = 0x222222;
    }
    task_list[tid].has_drawn = 1;
}

void compositor_task() {
    struct multiboot_info* mbi = VESA_get_boot_info();
    uint32_t sw = mbi->framebuffer_width;
    uint32_t sh = mbi->framebuffer_height;
    uint32_t* b_buffer = VESA_get_back_buffer();
    int last_gui_count = 0;

    while(1) {
        if (vesa_updating) { yield(); continue; }

        int gui_tasks  = 0;
        int needs_update = 0;
        for (int i = 0; i < MAX_TASKS; i++) {
            if (task_list[i].state != 0 && task_list[i].has_window) {
                gui_tasks++;
                if (task_list[i].has_drawn) needs_update = 1;
            }
        }

        if (gui_tasks != last_gui_count) {
            needs_update = 1;
            last_gui_count = gui_tasks;
            refresh_tiling_layout();
        }

        if (!needs_update) { sleep(10); continue; }

        if (gui_tasks == 0) {
            VESA_draw_rect(0, 0, sw, sh, 0x000033);
        } else {
            int tile_width = sw / gui_tasks;
            int current_tile = 0;

            for (int i = 0; i < MAX_TASKS; i++) {
                if (task_list[i].state != 0 && task_list[i].has_window) {
                    int start_x = current_tile * tile_width;

                    if (current_tile > 0) {
                        // Draw the 2px grey divider directly into b_buffer
                        for (uint32_t y = 0; y < sh; y++) {
                            b_buffer[y * sw + start_x]     = 0x888888;
                            b_buffer[y * sw + start_x + 1] = 0x888888;
                        }
                        start_x += 2; // shift the blit start past the divider
                    }

                    // Blit this tile's visible rows.
                    // src row starts at column 0 of the window_buffer
                    // (the buffer is full-screen width, so stride is sw).
                    // We copy exactly (tile_width - divider) pixels per row,
                    // which is also exactly the space we have in b_buffer.
                    int copy_w = tile_width - (current_tile > 0 ? 2 : 0);
                    for (uint32_t y = 0; y < sh; y++) {
                        uint32_t* src = &task_list[i].window_buffer[y * sw];
                        uint32_t* dst = &b_buffer[y * sw + start_x];
                        kmemcpy32(dst, src, copy_w);
                    }

                    task_list[i].has_drawn = 0;
                    current_tile++;
                }
            }
        }
vesa_dirty = 1;  // back_buffer now has fresh compositor output
        VESA_flip();
        sleep(10);
    }
}
