#include "task.h"
#include "vesa.h"
#include <stdint.h>
#include "kheap.h"
#include "io.h"
#include "shell.h"
#include "lib.h"
#include "fat.h"
#include "bmp.h"

uint32_t* desktop_bg_buffer = NULL; 
int pending_shell_spawn = 0;
extern uint32_t* VESA_get_back_buffer();
int keyboard_focus_tid = 0;
extern int vesa_updating;
extern int vesa_dirty; // Global link to VESA flag
extern void switch_to_stack(uint32_t* old_esp, uint32_t new_esp);
extern volatile uint32_t system_ticks;
int multitasking_enabled = 0;
volatile struct task task_list[MAX_TASKS];
int current_task_idx = 0;
extern volatile int klog_needs_sync;
extern char klog_ram_buffer[];

extern void mark_task_dirty(int id, int x, int y, int w, int h);


void shell_task() {
    char line[128];
    int idx = 0;

    VESA_print("KDXOS Shell Started.\n", COLOR_CYAN);
    VESA_print("> ", COLOR_YELLOW);
    
    // Force the whole window to redraw
    mark_task_dirty(current_task_idx, 0, 0, 4000, 4000); 

    while(1) {
        if (task_list[current_task_idx].cursor_x >= task_list[current_task_idx].win_w) {
            task_list[current_task_idx].cursor_x = 0;
            task_list[current_task_idx].cursor_y += 10;
        }

        char c = keyboard_getchar();

        if (c == 4) { 
            __asm__ volatile("mov $4, %%eax; int $0x80" : : : "eax");
            while(1) yield();
        }

        if (c == '\n') {
            line[idx] = '\0';
            VESA_print("\n", COLOR_WHITE);
            if (idx > 0) execute_command(line);
            idx = 0;
            VESA_print("> ", COLOR_YELLOW);
        }
        else if (c == '\b' && idx > 0) {
            idx--;
            if (task_list[current_task_idx].cursor_x >= 8) task_list[current_task_idx].cursor_x -= 0x08;
            VESA_draw_char(' ', task_list[current_task_idx].cursor_x, task_list[current_task_idx].cursor_y, 0x222222);
        }
        else if (idx < 127 && c >= ' ') {
            line[idx++] = c;
            char str[2] = {c, '\0'};
            VESA_print(str, COLOR_WHITE);
        }
        
        mark_task_dirty(current_task_idx, 0, 0, 4000, 4000); 
    }
}

int spawn_task(void (*entry_point)(), void* code_ptr, char* name) {
    for (int i = 1; i < MAX_TASKS; i++) {
        if (task_list[i].state == 0) {
            kstrncpy((char*)task_list[i].name, name, 15);
            task_list[i].name[15] = '\0';
            task_list[i].total_ticks = 0;
            task_list[i].sleep_ticks = 0;
            task_list[i].is_dirty = 0; 
            task_list[i].has_window = 0;
            task_list[i].window_buffer = NULL;
            task_list[i].kbd_head = 0; 
            task_list[i].kbd_tail = 0;

            uint32_t stack_size = 16384; 
            void* raw_stack = kmalloc(stack_size + 0x1000);
            if (!raw_stack) return -1;
            
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
        vesa_dirty = 1;
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
    task_list[id].is_dirty   = 0; 
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

    task_list[tid].win_x = x;
    task_list[tid].win_y = y;
    
    task_list[tid].win_w = (w == 0) ? (int)mbi->framebuffer_width : w;
    task_list[tid].win_h = (h == 0) ? (int)mbi->framebuffer_height : h;
    
    task_list[tid].cursor_x = 0;
    task_list[tid].cursor_y = 0;

    task_list[tid].window_buffer = (uint32_t*)kmalloc(full_size * 4);

    if (task_list[tid].window_buffer == NULL) {
        VESA_print("\n[OS] ERROR: Out of Memory for Task Window!\n", 0xFF0000);
        task_list[tid].has_window = 0; 
        return;
    }

    task_list[tid].has_window = 1;
    for (uint32_t p = 0; p < full_size; p++) {
        task_list[tid].window_buffer[p] = 0x222222;
    }
    
    refresh_tiling_layout();
    mark_task_dirty(tid, 0, 0, 4000, 4000); 
}

void run_startup_tests() {
    VESA_print("\n--- Running KDXOS System Diagnostics ---\n", 0x00FFFF);
    vesa_dirty = 1; VESA_flip(); 

    struct multiboot_info* mbi = VESA_get_boot_info();
    char num_buf[16];
    
    VESA_print("[INFO] VESA Resolution...     ", 0xFFFFFF);
    itoa(mbi->framebuffer_width, num_buf, 10);
    VESA_print(num_buf, 0xFF0000);
    VESA_print("x", 0xFF0000);
    itoa(mbi->framebuffer_height, num_buf, 10);
    VESA_print(num_buf, 0xFF0000);
    VESA_print("\n", 0xFFFFFF);
    vesa_dirty = 1; VESA_flip();

    VESA_print("[TEST] KHeap Integrity & Splitting... ", 0xFFFFFF);
    
    uint32_t* ptr1 = (uint32_t*)kmalloc(1024);
    uint32_t* ptr2 = (uint32_t*)kmalloc(2048);
    uint32_t* ptr3 = (uint32_t*)kmalloc(4096);

    if (!ptr1 || !ptr2 || !ptr3) {
        VESA_print("FAIL (Out of Memory)\n", 0xFF0000);
    } else {
        ptr1[0] = 0xDEADBEEF;
        ptr2[0] = 0xCAFEBABE;
        ptr3[0] = 0x12345678;

        if (ptr1[0] == 0xDEADBEEF && ptr2[0] == 0xCAFEBABE && ptr3[0] == 0x12345678) {
            kfree(ptr2); 
            uint32_t* ptr2_half = (uint32_t*)kmalloc(1024);
            
            if ((uint32_t)ptr2_half == (uint32_t)ptr2) {
                VESA_print("PASS\n", 0x00FF00);
            } else {
                VESA_print("WARN (Split Logic Failed)\n", 0xFFFF00);
            }
            kfree(ptr2_half);
        } else {
            VESA_print("FAIL (Memory Corruption Detected)\n", 0xFF0000);
        }
        kfree(ptr1);
        kfree(ptr3);
    }
    vesa_dirty = 1; VESA_flip();

    VESA_print("[TEST] FAT Disk I/O & Persistence...  ", 0xFFFFFF);
    
    char test_file[] = "DIAG.TXT";
    char test_data[] = "KDXOS_HARD_DISK_INTEGRITY_CHECK_PASS";

    fat_touch(test_file);
    fat_write_file(test_file, test_data);

    struct fat_dir_entry* entry = fat_search(test_file);
    if (entry) {
        char* loaded = (char*)fat_load_file(entry);
        if (loaded) {
            int match = 1;
            for (int i = 0; test_data[i] != '\0'; i++) {
                if (loaded[i] != test_data[i]) { 
                    match = 0; 
                    break; 
                }
            }

            if (match) VESA_print("PASS\n", 0x00FF00);
            else VESA_print("FAIL (Data Mismatch/Corruption)\n", 0xFF0000);

            kfree(loaded);
        } else {
            VESA_print("FAIL (Could not load file data)\n", 0xFF0000);
        }
        fat_rm(test_file);
    } else {
        VESA_print("FAIL (File Creation Error)\n", 0xFF0000);
    }

    VESA_print("--- Diagnostics Complete ---\n\n", 0x00FFFF);
    vesa_dirty = 1; VESA_flip(); 
    sleep(2000);
}

void init_multitasking() {
    __asm__ volatile("cli"); 

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
    kstrncpy((char*)task_list[0].name, "kernel", 15);

    struct multiboot_info* mbi = VESA_get_boot_info();
    uint32_t full_size = mbi->framebuffer_width * mbi->framebuffer_height;
    
    desktop_bg_buffer = (uint32_t*)kmalloc(full_size * 4);
    if (desktop_bg_buffer) {
        for(uint32_t p = 0; p < full_size; p++) desktop_bg_buffer[p] = 0x111111;
        load_desktop_wallpaper("BG.BMP");
    }

    int shell_tid = spawn_task(shell_task, NULL, "shell");
    task_create_window(shell_tid, 0, 0, 0, 0);
    keyboard_focus_tid = shell_tid;

    spawn_task(idle_task_code, NULL, "idle");
    spawn_task(compositor_task, NULL, "wm");
    spawn_task(klog_daemon, NULL, "log_daemon");

    current_task_idx = 0;
    refresh_tiling_layout();

    __asm__ volatile("sti"); 
}

void compositor_task() {
    struct multiboot_info* mbi = VESA_get_boot_info();
    uint32_t sw = mbi->framebuffer_width;
    uint32_t sh = mbi->framebuffer_height;
    uint32_t* b_buffer = VESA_get_back_buffer();
    int last_gui_count = -1; 

    if (desktop_bg_buffer) {
        kmemcpy32(b_buffer, desktop_bg_buffer, sw * sh);
    } else {
        for(uint32_t p=0; p < sw*sh; p++) b_buffer[p] = 0x000033;
    }
    vesa_dirty = 1;
    VESA_flip();

    while(1) {
        if (vesa_updating) { yield(); continue; }

        if (pending_shell_spawn) {
            pending_shell_spawn = 0;
            __asm__ volatile("cli"); 
            int new_tid = spawn_task(shell_task, NULL, "shell");
            if (new_tid != -1) {
                task_create_window(new_tid, 0, 0, 0, 0);
                keyboard_focus_tid = new_tid; 
            }
            __asm__ volatile("sti"); 
        }

        int gui_tasks = 0;
        int needs_update = 0;
        for (int i = 0; i < MAX_TASKS; i++) {
            if (task_list[i].state != 0 && task_list[i].has_window) {
                gui_tasks++;
                if (task_list[i].is_dirty) needs_update = 1;
            }
        }

        // --- FULL REDRAW ---
        if (gui_tasks != last_gui_count) {
            last_gui_count = gui_tasks;
            refresh_tiling_layout();
            
            if (desktop_bg_buffer) kmemcpy32(b_buffer, desktop_bg_buffer, sw * sh);
            else for(uint32_t p=0; p < sw*sh; p++) b_buffer[p] = 0x000033;

            int tile_width = (gui_tasks > 0) ? sw / gui_tasks : sw;
            int current_tile = 0;
            int border_thickness = 2;

            for (int i = 0; i < MAX_TASKS; i++) {
                if (task_list[i].state != 0 && task_list[i].has_window && task_list[i].window_buffer) {
                    int start_x = current_tile * tile_width;
                    int current_tile_w = (current_tile == gui_tasks - 1) ? (int)(sw - start_x) : tile_width;

                    for (uint32_t y = 0; y < sh; y++) {
                        kmemcpy32(&b_buffer[y * sw + start_x], 
                                  &task_list[i].window_buffer[y * sw], 
                                  current_tile_w);
                    }

                    uint32_t border_color = (i == keyboard_focus_tid) ? 0x00FFFF : 0x444444;
                    for (int by = 0; by < border_thickness; by++) {
                        for (int bx = 0; bx < current_tile_w; bx++) {
                            b_buffer[(by) * sw + start_x + bx] = border_color;
                            b_buffer[(sh - 1 - by) * sw + start_x + bx] = border_color;
                        }
                    }
                    for (uint32_t by = 0; by < sh; by++) {
                        for (int bx = 0; bx < border_thickness; bx++) {
                            b_buffer[by * sw + start_x + bx] = border_color;
                            b_buffer[by * sw + start_x + current_tile_w - 1 - bx] = border_color;
                        }
                    }
                    task_list[i].is_dirty = 0;
                    current_tile++;
                }
            }
            
            vesa_dirty = 1;
            VESA_flip();
            yield();
            continue;
        }

        // --- FAST PATH (BATCHED) ---
        if (needs_update) {
            int tile_width = (gui_tasks > 0) ? sw / gui_tasks : sw;
            int current_tile = 0;

            // Initialize Master Bounding Box parameters
            int min_x = sw, min_y = sh, max_x = 0, max_y = 0;
            int did_draw = 0;

            for (int i = 0; i < MAX_TASKS; i++) {
                if (task_list[i].state != 0 && task_list[i].has_window && task_list[i].window_buffer) {
                    
                    if (task_list[i].is_dirty) {
                        
                        int dx = task_list[i].dirty_x;
                        int dy = task_list[i].dirty_y;
                        int dw = task_list[i].dirty_w;
                        int dh = task_list[i].dirty_h;
                        
                        task_list[i].is_dirty = 0;

                        int start_x = current_tile * tile_width;
                        int current_tile_w = (current_tile == gui_tasks - 1) ? (int)(sw - start_x) : tile_width;
                        if (dx + dw > current_tile_w) dw = current_tile_w - dx;
                        if (dy + dh > (int)sh) dh = sh - dy;

                        if (dw > 0 && dh > 0) {
                            for (int row = 0; row < dh; row++) {
                                int win_offset = ((dy + row) * sw) + dx;
                                int b_buffer_offset = ((dy + row) * sw) + (start_x + dx);
                                kmemcpy32(&b_buffer[b_buffer_offset], 
                                          &task_list[i].window_buffer[win_offset], 
                                          dw);
                            }

                            uint32_t border_color = (i == keyboard_focus_tid) ? 0x00FFFF : 0x444444;
                            int border_thickness = 2;
                            for (int by = 0; by < border_thickness; by++) {
                                for (int bx = 0; bx < current_tile_w; bx++) {
                                    b_buffer[(by) * sw + start_x + bx] = border_color;
                                    b_buffer[(sh - 1 - by) * sw + start_x + bx] = border_color;
                                }
                            }
                            for (uint32_t by = 0; by < sh; by++) {
                                for (int bx = 0; bx < border_thickness; bx++) {
                                    b_buffer[by * sw + start_x + bx] = border_color;
                                    b_buffer[by * sw + start_x + current_tile_w - 1 - bx] = border_color;
                                }
                            }

                            // EXPAND THE MASTER BOUNDING BOX
                            if (start_x < min_x) min_x = start_x;
                            if (0 < min_y) min_y = 0; // Borders touch Y=0
                            if (start_x + current_tile_w > max_x) max_x = start_x + current_tile_w;
                            if ((int)sh > max_y) max_y = sh; // Borders touch Y=sh
                            
                            did_draw = 1;
                        }
                    }
                    current_tile++;
                }
            }
            
            // PERFORM ONE SINGLE UNIFIED VESA UPDATE
            if (did_draw) {
                VESA_update_rect(min_x, min_y, max_x - min_x, max_y - min_y);
            }
        }
        
        yield();
    }
}

void klog_daemon() {
    while(1) {
        if (klog_needs_sync) {
            if (!fat_search("KDX.LOG")) {
                fat_touch("KDX.LOG");
            }
            fat_append_file("KDX.LOG", klog_ram_buffer);
            kmemset(klog_ram_buffer, 0, 1024);
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
