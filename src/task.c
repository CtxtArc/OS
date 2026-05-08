#include "task.h"
#include "vesa.h"
#include <stdint.h>
#include "kheap.h"
#include "io.h"
#include "shell.h"
#include "lib.h"
#include "fat.h"
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
    task_list[0].has_drawn = 1; // Force the initial prompt to draw

    while(1) {
        // Line wrap logic
        if (task_list[0].cursor_x >= task_list[0].win_w) {
            task_list[0].cursor_x = 0;
            task_list[0].cursor_y += 10;
        }

        // keyboard_getchar() safely puts the Shell to sleep (0% CPU) 
        // and instantly wakes it up the millisecond a key is pressed!
        char c = keyboard_getchar();

        if (c == '\n') {
            line[idx] = '\0';
            VESA_print("\n", COLOR_WHITE);
            if (idx > 0) execute_command(line);
            idx = 0;
            VESA_print("> ", COLOR_YELLOW);
        }
        else if (c == '\b' && idx > 0) {
            idx--;
            if (task_list[0].cursor_x >= 8) task_list[0].cursor_x -= 0x08;
            VESA_draw_char(' ', task_list[0].cursor_x, task_list[0].cursor_y, 0x222222);
        }
        else if (idx < 127 && c >= ' ') {
            line[idx++] = c;
            char str[2] = {c, '\0'};
            VESA_print(str, COLOR_WHITE);
        }
        
        // --- THE FIX ---
        // Tell the Compositor to push whatever we just did to the screen immediately!
        task_list[0].has_drawn = 1;  
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

            // FIX #1: INCREASED STACK TO 16KB TO PREVENT REBOOTS!
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

            // POINT TO THE NEW 16KB ROOF
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
int task_is_ready(int id) { return (id >= 0 && id < MAX_TASKS) ? (task_list[id].state == 1) : 0; }
uint32_t task_get_esp(int id) { return (id >= 0 && id < MAX_TASKS) ? task_list[id].esp : 0; }
char* task_get_name(int id) { return (id >= 0 && id < MAX_TASKS) ? (char*)task_list[id].name : "unused"; }
int task_get_state(int id) { return (id >= 0 && id < MAX_TASKS) ? task_list[id].state : -1; }
int task_get_sleep_ticks(int id) { return (id >= 0 && id < MAX_TASKS) ? task_list[id].sleep_ticks : -1; }
int task_get_total_ticks(int id) { return (id >= 0 && id < MAX_TASKS) ? task_list[id].total_ticks : -1; }

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

    task_list[tid].win_x = x;
    task_list[tid].win_y = y;
    
    // SAFETY: Never allow a window width/height of 0. Default to screen size temporarily.
    task_list[tid].win_w = (w == 0) ? mbi->framebuffer_width : w;
    task_list[tid].win_h = (h == 0) ? mbi->framebuffer_height : h;
    
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
    
    // FIX: Force the tiling manager to split the screen RIGHT NOW, 
    // before the new task has a chance to execute and draw out of bounds!
    refresh_tiling_layout();
    
    task_list[tid].has_drawn = 1;
}
void run_startup_tests() {
    VESA_print("\n--- Running KDXOS System Diagnostics ---\n", 0x00FFFF);

    // ==========================================
    // TEST 1: KERNEL HEAP ALLOCATOR (kheap)
    // ==========================================
    VESA_print("[TEST] KHeap Integrity & Splitting... ", 0xFFFFFF);
    
    // 1. Allocate three adjacent blocks
    uint32_t* ptr1 = (uint32_t*)kmalloc(1024);
    uint32_t* ptr2 = (uint32_t*)kmalloc(2048);
    uint32_t* ptr3 = (uint32_t*)kmalloc(4096);

    if (!ptr1 || !ptr2 || !ptr3) {
        VESA_print("FAIL (Out of Memory)\n", 0xFF0000);
    } else {
        // 2. Write Magic Numbers to ensure no overlaps
        ptr1[0] = 0xDEADBEEF;
        ptr2[0] = 0xCAFEBABE;
        ptr3[0] = 0x12345678;

        if (ptr1[0] == 0xDEADBEEF && ptr2[0] == 0xCAFEBABE && ptr3[0] == 0x12345678) {
            
            // 3. Test the "Healer Split" logic
            kfree(ptr2); // Free the 2048-byte middle block
            
            // Request 1024 bytes. It should split the old ptr2 block and give us the exact same address back!
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
        
        // 4. Free remaining blocks to test coalescing
        kfree(ptr1);
        kfree(ptr3);
    }

    // ==========================================
    // TEST 2: FAT FILESYSTEM (Read/Write/Search)
    // ==========================================
    VESA_print("[TEST] FAT Disk I/O & Persistence...  ", 0xFFFFFF);
    
    char test_file[] = "DIAG.TXT";
    char test_data[] = "KDXOS_HARD_DISK_INTEGRITY_CHECK_PASS";

    // 1. Create and Write
    fat_touch(test_file);
    fat_write_file(test_file, test_data);

    // 2. Search for the newly created file
    struct fat_dir_entry* entry = fat_search(test_file);
    if (entry) {
        // 3. Read it back from the IDE disk
        char* loaded = (char*)fat_load_file(entry);
        if (loaded) {
            // 4. Verify contents byte-by-byte
            int match = 1;
            for (int i = 0; test_data[i] != '\0'; i++) {
                if (loaded[i] != test_data[i]) { 
                    match = 0; 
                    break; 
                }
            }

            if (match) {
                VESA_print("PASS\n", 0x00FF00);
            } else {
                VESA_print("FAIL (Data Mismatch/Corruption)\n", 0xFF0000);
            }

            kfree(loaded);
        } else {
            VESA_print("FAIL (Could not load file data)\n", 0xFF0000);
        }
        
        // 5. Clean up so we don't clutter the disk on every boot
        fat_rm(test_file);
    } else {
        VESA_print("FAIL (File Creation Error)\n", 0xFF0000);
    }

    VESA_print("--- Diagnostics Complete ---\n\n", 0x00FFFF);
    sleep(2000);
}
void compositor_task() {
    struct multiboot_info* mbi = VESA_get_boot_info();
    uint32_t sw = mbi->framebuffer_width;
    uint32_t sh = mbi->framebuffer_height;
    uint32_t* b_buffer = VESA_get_back_buffer();
    int last_gui_count = -1; 

    while(1) {
        if (vesa_updating) { yield(); continue; }

        int gui_tasks = 0;
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

        if (!needs_update) { 
            // FIX: Yield instead of sleep to prevent input lag!
            yield(); 
            continue; 
        }

        if (gui_tasks == 0) {
            VESA_draw_rect(0, 0, sw, sh, 0x000033);
        } else {
            // WIPE back buffer background to cleanly reset edges
            for(uint32_t p=0; p < sw*sh; p++) b_buffer[p] = 0x000033;

            int tile_width = sw / gui_tasks;
            int current_tile = 0;
            int border_thickness = 2; // A crisp, clean 2-pixel border

            for (int i = 0; i < MAX_TASKS; i++) {
                if (task_list[i].state != 0 && task_list[i].has_window && task_list[i].window_buffer != NULL) {
                    
                    int start_x = current_tile * tile_width;
                    int current_tile_w = tile_width;

                    // Ensure the last tile stretches perfectly to the right wall
                    if (current_tile == gui_tasks - 1) {
                        current_tile_w = sw - start_x;
                    }

                    // 1. Blit the actual window contents seamlessly
                    for (uint32_t y = 0; y < sh; y++) {
                        uint32_t* src = &task_list[i].window_buffer[y * sw];
                        uint32_t* dst = &b_buffer[y * sw + start_x];
                        kmemcpy32(dst, src, current_tile_w);
                    }

                    // 2. THE TWM BORDER LOGIC
                    // Active window gets Bright Cyan, Inactive gets Dark Gray
                    uint32_t border_color = (i == keyboard_focus_tid) ? 0x00FFFF : 0x444444;

                    // Draw Top & Bottom Edges
                    for (int by = 0; by < border_thickness; by++) {
                        for (int bx = 0; bx < current_tile_w; bx++) {
                            b_buffer[(by) * sw + start_x + bx]          = border_color; // Top
                            b_buffer[(sh - 1 - by) * sw + start_x + bx] = border_color; // Bottom
                        }
                    }
                    
                    // Draw Left & Right Edges
                    for (uint32_t by = 0; by < sh; by++) {
                        for (int bx = 0; bx < border_thickness; bx++) {
                            b_buffer[by * sw + start_x + bx]                      = border_color; // Left
                            b_buffer[by * sw + start_x + current_tile_w - 1 - bx] = border_color; // Right
                        }
                    }

                    task_list[i].has_drawn = 0;
                    current_tile++;
                }
            }
        }
        
        vesa_dirty = 1; 
        VESA_flip();
        
        // FIX: Yield instead of sleep to run at max FPS!
        yield();
    }
}
