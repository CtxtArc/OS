#include "task.h"
#include "vesa.h"
#include <stdint.h>
#include "kheap.h"
#include "io.h"
#include "shell.h"
#include "lib.h"
#include "fat.h"
#include "bmp.h"
#include "vfs.h"
#include "wm.h"

#define TEST_LABEL_X 100
#define TEST_RESULT_X 450 

char shell_history[10][128];
int history_count = 0;
int history_write_idx = 0;
int history_view_idx = -1;

uint32_t* desktop_bg_buffer = NULL; 
int pending_shell_spawn = 1;
extern uint32_t* VESA_get_back_buffer();
volatile int keyboard_focus_tid = 0;
extern int vesa_updating;
extern int vesa_dirty; 
extern void switch_to_stack(uint32_t* old_esp, uint32_t new_esp);
extern volatile uint32_t system_ticks;
volatile int multitasking_enabled = 0;
volatile struct task task_list[MAX_TASKS];
volatile int current_task_idx = 0;
extern volatile int klog_needs_sync;
extern char klog_ram_buffer[];
extern uint8_t font8x8_basic[128][8];



void schedule() {
    if (!multitasking_enabled) return;

    int old_idx = current_task_idx;
    int next_idx = current_task_idx;

    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_list[i].state == 2) { 
            if (task_list[i].sleep_ticks > 0) {
                task_list[i].sleep_ticks--;
            } else {
                task_list[i].state = 1; 
            }
        }
    }

    do {
        next_idx++;
        if (next_idx >= MAX_TASKS) next_idx = 0;
    } while (task_list[next_idx].state != 1 && next_idx != old_idx);

    if (next_idx != old_idx) {
        current_task_idx = next_idx;
        task_list[next_idx].total_ticks++;
        switch_to_stack((uint32_t*)&task_list[old_idx].esp, task_list[next_idx].esp);
    }
}

int spawn_task(void (*entry_point)(), void* code_ptr, char* name) {
    if (!entry_point) return ERR_TASK_INVALID_EP;

    for (int i = 1; i < MAX_TASKS; i++) {
        if (task_list[i].state == 0) {
            kstrncpy((char*)task_list[i].name, name, 15);
            task_list[i].name[15] = '\0';
            
            task_list[i].has_window = 0;
            task_list[i].window_ready = 0; 
            task_list[i].window_buffer = NULL;
            task_list[i].total_ticks = 0;
            task_list[i].sleep_ticks = 0;
            task_list[i].is_dirty = 0; 
            task_list[i].kbd_head = 0; 
            task_list[i].kbd_tail = 0;

            uint32_t stack_size = 16384; 
            void* raw_stack = kmalloc(stack_size + 0x1000);
            if (!raw_stack) return ERR_TASK_STACK_OOM;
            
            task_list[i].stack_ptr = raw_stack;
            task_list[i].code_ptr = code_ptr;

            uint32_t aligned_stack = (uint32_t)raw_stack;
            if (aligned_stack & 0xFFF) {
                aligned_stack &= 0xFFFFF000;
                aligned_stack += 0x1000;
            }

            uint32_t* s_ptr = (uint32_t*)(aligned_stack + stack_size);
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
    return ERR_TASK_TABLE_FULL;
}

void kill_task(int id) {
    if (id <= 0 || id >= MAX_TASKS) return;

    // 1. Stop the compositor from looking at this window immediately
    task_list[id].window_ready = 0;
    task_list[id].has_window = 0;

    // 2. Free the memory
    if (task_list[id].window_buffer) {
        kfree(task_list[id].window_buffer);
        task_list[id].window_buffer = NULL;
    }
    if (task_list[id].stack_ptr) { kfree(task_list[id].stack_ptr); task_list[id].stack_ptr = NULL; }
    if (task_list[id].code_ptr)  { kfree(task_list[id].code_ptr);  task_list[id].code_ptr  = NULL; }

    // 3. Mark state as dead
    task_list[id].state = 0;

    // 4. Trigger the Compositor to rearrange remaining windows
    refresh_tiling_layout();
    
    // 5. IMPORTANT: Force a full global redraw to clean up the "hole" left behind
    for(int i = 0; i < MAX_TASKS; i++) {
        if (task_list[i].state != 0) {
            mark_task_dirty(i, 0, 0, 4000, 4000);
        }
    }
}

void shell_task() {
    while (!task_list[current_task_idx].has_window || !task_list[current_task_idx].window_ready) {
        yield(); 
    }
    load_history();
    char line[128];
    int idx = 0;

    task_list[current_task_idx].cursor_x = WIN_BORDER + 2;
    task_list[current_task_idx].cursor_y = WIN_BORDER + 2;

    shell_print("> ", 0xFFFF00); 
    mark_task_dirty(current_task_idx, 0, 0, 4000, 4000); 

    while(1) {
        volatile struct task* t = &task_list[current_task_idx];

        if (t->kbd_head == t->kbd_tail) {
            yield(); 
            continue;
        }

        char c = t->kbd_buffer[t->kbd_tail];
        t->kbd_tail = (t->kbd_tail + 1) % 64;

        // Handle typing wrapping around the right edge and scrolling
        if (t->cursor_x >= t->win_w - 8) {
            t->cursor_x = WIN_BORDER + 2;
            t->cursor_y += 10;
            if (t->cursor_y + 10 >= t->win_h) shell_scroll(); // Trigger Scroll
        }

        if (c == 4) { 
            __asm__ volatile("mov $4, %%eax; int $0x80" : : : "eax");
            while(1) yield();
        }

        // --- ARROW UP ---
        if (c == 11 && history_count > 0) {
            if (history_view_idx == -1) history_view_idx = history_count - 1;
            else if (history_view_idx > 0) history_view_idx--;

            // Clear current line on screen
            while(idx > 0) { idx--; t->cursor_x -= 8; shell_draw_char(' ', t->cursor_x, t->cursor_y, 0x222222, 0x222222); }
            
            // Copy history to current line
            int actual_idx = (history_count == MAX_HISTORY) ? (history_write_idx + history_view_idx) % MAX_HISTORY : history_view_idx;
            kstrcpy(line, shell_history[actual_idx]);
            idx = kstrlen(line);
            shell_print(line, 0xFFFFFF);
            mark_task_dirty(current_task_idx, 0, 0, 4000, 4000);
        }
        // --- ARROW DOWN ---
        else if (c == 12) {
            if (history_view_idx != -1) {
                history_view_idx++;
                if (history_view_idx >= history_count) {
                    history_view_idx = -1; // Return to empty line
                    while(idx > 0) { idx--; t->cursor_x -= 8; shell_draw_char(' ', t->cursor_x, t->cursor_y, 0x222222, 0x222222); }
                } else {
                    while(idx > 0) { idx--; t->cursor_x -= 8; shell_draw_char(' ', t->cursor_x, t->cursor_y, 0x222222, 0x222222); }
                    int actual_idx = (history_count == MAX_HISTORY) ? (history_write_idx + history_view_idx) % MAX_HISTORY : history_view_idx;
                    kstrcpy(line, shell_history[actual_idx]);
                    idx = kstrlen(line);
                    shell_print(line, 0xFFFFFF);
                }
                mark_task_dirty(current_task_idx, 0, 0, 4000, 4000);
            }
        }
        // --- ENTER (Process & Save History) ---
        else if (c == '\n') {
            line[idx] = '\0';
            shell_print("\n", 0xFFFFFF);
            if (idx > 0) {
                // Add to circular buffer if not identical to last entry
                kstrcpy(shell_history[history_write_idx], line);
                history_write_idx = (history_write_idx + 1) % MAX_HISTORY;
                if (history_count < MAX_HISTORY) history_count++;
                
                save_history_to_disk();
                execute_command(line);
            }
            idx = 0;
            history_view_idx = -1;
            shell_print("> ", 0xFFFF00);
            mark_task_dirty(current_task_idx, 0, 0, 4000, 4000);
        }
        // --- BACKSPACE / CHARS ---
        else if (c == '\b' && idx > 0) {
            idx--; t->cursor_x -= 8;
            shell_draw_char(' ', t->cursor_x, t->cursor_y, 0x222222, 0x222222);
            mark_task_dirty(current_task_idx, t->cursor_x, t->cursor_y, 8, 8);
        }
        else if (idx < 127 && c >= ' ' && c <= '~') {
            line[idx++] = c;
            char str[2] = {c, '\0'};
            int sx = t->cursor_x; int sy = t->cursor_y;
            shell_print(str, 0xFFFFFF);
            mark_task_dirty(current_task_idx, sx, sy, 8, 8);
        }
        else if (idx < 127 && c >= ' ') {
            line[idx++] = c;
            char str[2] = {c, '\0'};
            
            int start_x = t->cursor_x;
            int start_y = t->cursor_y;
            shell_print(str, 0xFFFFFF);
            
            mark_task_dirty(current_task_idx, start_x, start_y, 8, 8); 
        }
    }
}

uint32_t task_stacks[MAX_TASKS][1024];

void yield() {
    __asm__ volatile("hlt");
}

void idle_task_code() {
    while(1) __asm__ volatile("hlt");
}

int get_current_task_id() { return current_task_idx; }
int task_is_ready(int id) { return (id >= 0 && id < MAX_TASKS) ? (task_list[id].state == 1) : 0; }
uint32_t task_get_esp(int id) { return (id >= 0 && id < MAX_TASKS) ? task_list[id].esp : 0; }
char* task_get_name(int id) { return (id >= 0 && id < MAX_TASKS) ? (char*)task_list[id].name : "unused"; }

int task_get_state(int id) { return (id >= 0 && id < MAX_TASKS) ? (int)task_list[id].state : -1; }
int task_get_sleep_ticks(int id) { return (id >= 0 && id < MAX_TASKS) ? (int)task_list[id].sleep_ticks : -1; }
int task_get_total_ticks(int id) { return (id >= 0 && id < MAX_TASKS) ? (int)task_list[id].total_ticks : -1; }

void task_timer() {
    uint32_t seconds = 0;
    while (1) {
        seconds++;
        char buf[20];
        
        kmemset(buf, 0, 20 / 4); 
        kstrcpy(buf, "TIMER: ");
        itoa(seconds, buf + 7, 10);
        VESA_print_at(buf, 900, 10, 0x00FFFF);
        sleep(1000);
    }
}

void task_game() {
    int previous_focus = keyboard_focus_tid;
    keyboard_focus_tid = current_task_idx;

    while (has_key_in_buffer()) get_key_from_buffer();

    vesa_updating = 1;
    VESA_clear();
    vesa_updating = 0;

    int x = 500, y = 500;
    int old_x = 500, old_y = 500;
    int exit = 0;

    VESA_draw_char('*', x, y, 0x00FFFF);
    vesa_dirty = 1;
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
            vesa_dirty = 1;
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

    while (has_key_in_buffer()) get_key_from_buffer();
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
        vesa_dirty = 1;
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
    vesa_dirty = 1;
    VESA_flip();
}


void run_startup_tests() {
    uint32_t color_info   = 0x00FFFF; 
    uint32_t color_title  = 0xFFFF00; 
    uint32_t color_pass   = 0x00FF00; 
    uint32_t color_fail   = 0xFF0000; 
    uint32_t color_text   = 0xFFFFFF; 
    uint32_t desktop_blue = 0x000033; 

    VESA_draw_rect(0, 0, 1024, 768, desktop_blue);

    int cur_y = 150;
    VESA_print_at("===================================================", TEST_LABEL_X, cur_y, color_title); cur_y += 20;
    VESA_print_at("            KDXOS SYSTEM DIAGNOSTICS                ", TEST_LABEL_X, cur_y, color_title); cur_y += 20;
    VESA_print_at("===================================================", TEST_LABEL_X, cur_y, color_title); cur_y += 40;
    
    struct multiboot_info* mbi = VESA_get_boot_info();
    char num_buf_w[16], num_buf_h[16];
    itoa(mbi->framebuffer_width, num_buf_w, 10);
    itoa(mbi->framebuffer_height, num_buf_h, 10);
    
    VESA_print_at("[INFO] VESA Resolution:", TEST_LABEL_X, cur_y, color_text);
    VESA_print_at(num_buf_w, TEST_RESULT_X, cur_y, color_info);
    int x_pos = TEST_RESULT_X + (kstrlen(num_buf_w) * 8) + 4;
    VESA_print_at("x", x_pos, cur_y, color_text);
    VESA_print_at(num_buf_h, x_pos + 12, cur_y, color_info);
    cur_y += 25;

    VESA_print_at("[TEST] KHeap Integrity:", TEST_LABEL_X, cur_y, color_text);
    void* p1 = kmalloc(512);
    if (p1) {
        VESA_print_at("[ PASS ]", TEST_RESULT_X, cur_y, color_pass);
        kfree(p1);
    } else {
        VESA_print_at("[ FAIL ]", TEST_RESULT_X, cur_y, color_fail);
    }
    cur_y += 25;

    VESA_print_at("[TEST] FAT File System:", TEST_LABEL_X, cur_y, color_text);
    if (fat_search("BG.BMP")) {
        VESA_print_at("[ PASS ]", TEST_RESULT_X, cur_y, color_pass);
    } else {
        VESA_print_at("[ FAIL ]", TEST_RESULT_X, cur_y, color_fail);
    }
    cur_y += 25;

    VESA_print_at("[TEST] Virtual File System:", TEST_LABEL_X, cur_y, color_text);
    if (fs_root != NULL) {
        vfs_node_t* test_node = vfs_finddir(fs_root, "BG.BMP");
        if (test_node && test_node->read != 0) {
            uint8_t magic[3];
            vfs_read(test_node, 0, 2, magic);
            magic[2] = '\0'; 
            if (magic[0] == 'B' && magic[1] == 'M') {
                VESA_print_at("[ PASS ]", TEST_RESULT_X, cur_y, color_pass);
            } else {
                VESA_print_at("[ FAIL ]", TEST_RESULT_X, cur_y, color_fail);
            }
            kfree(test_node); 
        } else {
            VESA_print_at("[ FAIL ]", TEST_RESULT_X, cur_y, color_fail);
            if (test_node) kfree(test_node);
        }
    } else {
        VESA_print_at("[ FAIL ]", TEST_RESULT_X, cur_y, color_fail);
    }
    cur_y += 50;

    VESA_print_at("DIAGNOSTICS COMPLETE.", TEST_LABEL_X, cur_y, color_info); cur_y += 25;
    VESA_print_at("PRESS ANY KEY TO CONTINUE...", TEST_LABEL_X, cur_y, color_title);
    
    vesa_dirty = 1; 
    VESA_flip();

    // Clear any existing keys first
    while(has_key_in_buffer()) get_key_from_buffer();
    // Wait for the user
    while(!has_key_in_buffer()) {
        __asm__ volatile("hlt");
    }
    get_key_from_buffer(); // Consume the key
}

void init_multitasking() {
    multitasking_enabled = 0; 

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
    kstrncpy((char*)task_list[0].name, "kernel", 15);

    struct multiboot_info* mbi = VESA_get_boot_info();
    uint32_t full_size = mbi->framebuffer_width * mbi->framebuffer_height;
    
    desktop_bg_buffer = (uint32_t*)kmalloc(full_size * 4);
    if (desktop_bg_buffer) {
        for(uint32_t p = 0; p < full_size; p++) desktop_bg_buffer[p] = 0x111111;
        load_desktop_wallpaper("BG.BMP");
    }

    int t1 = spawn_task(idle_task_code, NULL, "idle");
    int t2 = spawn_task(compositor_task, NULL, "wm");
    int t3 = spawn_task(klog_daemon, NULL, "log_daemon");

    if (t1 < 0 || t2 < 0 || t3 < 0) {
        VESA_print_at("FATAL ERROR: KHEAP OOM. TASKS FAILED TO SPAWN!", 10, 10, 0xFF0000);
        VESA_flip();
        while(1) __asm__ volatile("hlt");
    }

    current_task_idx = 0;
    refresh_tiling_layout();
    multitasking_enabled = 1; 
}

void klog_daemon() {
    while(1) {
        if (klog_needs_sync) {
            struct fat_dir_entry* log_file = fat_search("KDX.LOG");
            if (!log_file) fat_touch("KDX.LOG");
            
            fat_append_file("KDX.LOG", klog_ram_buffer);
            
            kmemset(klog_ram_buffer, 0, 1024 / 4);
            klog_needs_sync = 0;
        }
        sleep(2000); 
    }
}

void suicide_task() {
    VESA_print_at("Crash Task: I'm about to die...", 100, 100, 0xFFFF00);
    sleep(2000); 

    __asm__ volatile("mov $0xFFFF, %%ax; mov %%ax, %%ds" : : : "eax");

    while(1) yield();
}

void load_history() {
    struct fat_dir_entry* entry = fat_search("HISTORY.TXT");
    if (entry) {
        char* data = fat_load_file(entry);
        if (data) {
            uint32_t pos = 0;
            while (pos < entry->size && history_count < MAX_HISTORY) {
                int line_len = 0;
                while (data[pos + line_len] != '\n' && data[pos + line_len] != '\0') line_len++;
                
                kstrncpy(shell_history[history_write_idx], &data[pos], (line_len > 127) ? 127 : line_len);
                shell_history[history_write_idx][line_len] = '\0';
                
                history_write_idx = (history_write_idx + 1) % MAX_HISTORY;
                history_count++;
                pos += (line_len + 1);
            }
            kfree(data);
        }
        kfree(entry);
    }
}

void save_history_to_disk() {
    char* big_buf = kmalloc(MAX_HISTORY * 128);
    big_buf[0] = '\0';
    
    // Write oldest to newest
    int start = (history_count == MAX_HISTORY) ? history_write_idx : 0;
    for (int i = 0; i < history_count; i++) {
        int idx = (start + i) % MAX_HISTORY;
        kstrcat(big_buf, shell_history[idx]);
        kstrcat(big_buf, "\n");
    }
    
    if (!fat_search("HISTORY.TXT")) fat_touch("HISTORY.TXT");
    fat_write_file("HISTORY.TXT", big_buf);
    kfree(big_buf);
}
