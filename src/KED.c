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

// Link our dirty tracker helper
extern void mark_task_dirty(int id, int x, int y, int w, int h);

static char ked_target_file[16];

void KED_init(const char* filename) {
    kstrncpy(ked_target_file, filename, 15);
    ked_target_file[15] = '\0';
}

void KED_task() {
    keyboard_focus_tid = current_task_idx;
    
    char* text_buffer = (char*)kmalloc(4096);
    if (!text_buffer) {
        __asm__ volatile("mov $4, %%eax; int $0x80" : : : "eax");
        while(1) yield();
    }
    kmemset(text_buffer, 0, 1024); // 4096 bytes

    struct fat_dir_entry* entry = fat_search(ked_target_file);
    uint32_t total_len = 0;
    
    if (entry) {
        char* loaded_data = fat_load_file(entry);
        if (loaded_data) {
            total_len = (entry->size > 4094) ? 4094 : entry->size;
            kmemcpy(text_buffer, loaded_data, total_len);
            kfree(loaded_data);
        }
    } else {
        fat_touch(ked_target_file);
    }

    uint32_t cursor_index = total_len; // Logical position in the text
    int needs_redraw = 1;
    int last_cursor_state = -1;

    while (1) {
        if (has_key_in_buffer()) {
            char c = get_key_from_buffer();
            needs_redraw = 1;

            if (c == 17) break; // Ctrl+Q
            if (c == 19) {      // Ctrl+S
                fat_write_file(ked_target_file, text_buffer);
                break;
            }

            // --- NAVIGATION ---
            if (c == 13 && cursor_index > 0) { // LEFT
                cursor_index--;
            }
            else if (c == 14 && cursor_index < total_len) { // RIGHT
                cursor_index++;
            }
            else if (c == 11) { // UP (Find previous newline)
                if (cursor_index > 0) {
                    int i = cursor_index - 1;
                    while (i > 0 && text_buffer[i] != '\n') i--;
                    cursor_index = i;
                }
            }
            else if (c == 12) { // DOWN (Find next newline)
                int i = cursor_index;
                while (i < (int)total_len && text_buffer[i] != '\n') i++;
                if (i < (int)total_len) cursor_index = i + 1;
            }

            // --- DELETION ---
            else if (c == '\b' && cursor_index > 0) {
                // Shift memory left: [A B C | D E] -> [A B | D E]
                for (uint32_t i = cursor_index - 1; i < total_len; i++) {
                    text_buffer[i] = text_buffer[i + 1];
                }
                cursor_index--;
                total_len--;
            }

            // --- INSERTION ---
            else if ((c >= ' ' || c == '\n') && total_len < 4094) {
                // Shift memory right to make a hole: [A B | C D] -> [A B _ C D]
                for (uint32_t i = total_len; i > cursor_index; i--) {
                    text_buffer[i] = text_buffer[i - 1];
                }
                text_buffer[cursor_index] = c;
                cursor_index++;
                total_len++;
                text_buffer[total_len] = '\0';
            }
        }

        // --- RENDERING ---
        int current_cursor_state = (system_ticks / 500) % 2;
        if (current_cursor_state != last_cursor_state) {
            needs_redraw = 1;
            last_cursor_state = current_cursor_state;
        }

        if (needs_redraw) {
            __asm__ volatile("cli");
            VESA_clear_buffer_only();
            
            VESA_print_unsync("KED Editor - Editing: ", 0xFFFFFF);
            VESA_print_unsync(ked_target_file, 0x00FFFF);
            VESA_print_unsync("\nArrows: Move | Ctrl+S: Save | Ctrl+Q: Exit\n", 0xAAAAAA);
            VESA_print_unsync("-------------------------------------------\n", 0x888888);
            
            // Print text before cursor
            char saved = text_buffer[cursor_index];
            text_buffer[cursor_index] = '\0';
            VESA_print_unsync(text_buffer, 0xFFFFFF);
            text_buffer[cursor_index] = saved;

            // Draw Cursor
            if (current_cursor_state == 0) {
                VESA_print_unsync("_", 0x00FFFF);
            } else {
                char blink[2] = { (saved ? (saved == '\n' ? ' ' : saved) : ' '), '\0' };
                VESA_print_unsync(blink, 0xFFFFFF);
            }

            // Print text after cursor (adjusting for the character we highlighted)
            if (saved != '\0') {
                VESA_print_unsync(&text_buffer[cursor_index + (current_cursor_state == 0 ? 0 : 1)], 0xFFFFFF);
            }

            mark_task_dirty(current_task_idx, 0, 0, 4000, 4000); 
            needs_redraw = 0;
            __asm__ volatile("sti");
        }
        yield(); 
    }

    kfree(text_buffer);
    keyboard_focus_tid = 1; 
    __asm__ volatile("mov $4, %%eax; int $0x80" : : : "eax");
    while(1) yield(); 
}
