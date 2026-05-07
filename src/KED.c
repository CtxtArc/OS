#include "fat.h"
#include "vesa.h"
#include "kheap.h"
#include "io.h"
#include "lib.h"
#include "task.h"

extern int keyboard_focus_tid;
extern uint32_t system_ticks;
extern int current_task_idx;
extern volatile struct task task_list[]; 

static char ked_target_file[16];

void KED_init(const char* filename) {
    kstrncpy(ked_target_file, filename, 15);
    ked_target_file[15] = '\0';
}

void KED_task() {
    keyboard_focus_tid = current_task_idx;
    
    char* text_buffer = (char*)kmalloc(4096);
    if (!text_buffer) {
        // Safe exit via Syscall 4 (EXIT)
        __asm__ volatile("mov $4, %%eax; int $0x80" : : : "eax");
        while(1) yield();
    }
    
    // Fill the 4KB buffer with null terminators
    kmemset(text_buffer, 0, 4096 / 4);

    struct fat_dir_entry* entry = fat_search(ked_target_file);
    uint32_t cursor_pos = 0;
    
    if (entry) {
        char* loaded_data = fat_load_file(entry);
        if (loaded_data) {
            // Leave at least 1 byte for the null terminator
            uint32_t to_copy = (entry->size > 4095) ? 4095 : entry->size;
            kmemcpy(text_buffer, loaded_data, to_copy);
            cursor_pos = to_copy;
            kfree(loaded_data);
        }
    } else {
        fat_touch(ked_target_file);
    }

    // --- ANTI-BLINK VARIABLES ---
    int needs_redraw = 1;
    int last_cursor_state = -1;

    while (1) {
        // 1. Check if the cursor needs to blink
        int current_cursor_state = (system_ticks / 20) % 2;
        if (current_cursor_state != last_cursor_state) {
            needs_redraw = 1;
            last_cursor_state = current_cursor_state;
        }

        // 2. ONLY draw if something actually changed!
        if (needs_redraw) {
            VESA_clear_buffer_only();
            
            // Use VESA_print_unsync directly to avoid vsprintf buffer overflows!
            VESA_print_unsync("KED Editor - Editing: ", 0xFFFFFF);
            VESA_print_unsync(ked_target_file, 0x00FFFF);
            VESA_print_unsync("\nCtrl+S: Save & Exit | Ctrl+Q: Cancel\n", 0xAAAAAA);
            VESA_print_unsync("-------------------------------------------\n", 0x888888);
            
            // Safely print the massive buffer directly to the screen
            VESA_print_unsync(text_buffer, 0xFFFFFF);
            
            if (current_cursor_state == 0) {
                VESA_print_unsync("_", 0xFFFFFF);
            }

            task_list[current_task_idx].has_drawn = 1;
            needs_redraw = 0; // Reset flag so we don't draw next frame
        }

        // 3. Handle Input
        if (has_key_in_buffer()) {
            char c = get_key_from_buffer();
            
            needs_redraw = 1; // A key was pressed! Force a redraw!

            if (c == 17) { // Ctrl + Q (Cancel)
                break;
            }
            if (c == 19) { // Ctrl + S (Save)
                fat_write_file(ked_target_file, text_buffer);
                break; 
            }
            if (c == 16) { // Ctrl + P (Paste test block)
                uint32_t paste_size = 512;
                if (cursor_pos + paste_size < 4094) {
                    for (uint32_t i = 0; i < paste_size; i++) {
                        text_buffer[cursor_pos++] = 'H';
                    }
                    text_buffer[cursor_pos] = '\0';
                }
                continue;
            }
            if (c == '\b' && cursor_pos > 0) {
                text_buffer[--cursor_pos] = '\0';
            } 
            else if (c >= ' ' || c == '\n') {
                if (cursor_pos < 4094) {
                    text_buffer[cursor_pos++] = c;
                    text_buffer[cursor_pos] = '\0';
                }
            }
        }
        
        yield(); 
    }

    // Free the heap data before exiting
    kfree(text_buffer);
    
    // Return keyboard focus to the Shell
    keyboard_focus_tid = 0; 
    
    // Call Syscall 4 (EXIT) to let the Kernel safely reap the task
    __asm__ volatile("mov $4, %%eax; int $0x80" : : : "eax");
    
    // Wait for the scheduler to swap us out permanently
    while(1) yield(); 
}
