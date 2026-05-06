#include "fat.h"
#include "vesa.h"
#include "kheap.h"
#include "io.h"
#include "lib.h"
#include "task.h"

extern int vesa_updating;
extern int keyboard_focus_tid;
extern uint32_t system_ticks;
extern int current_task_idx;

void run_editor(const char* filename) {
    // 1. Setup focus and memory
    int previous_focus = keyboard_focus_tid;
    keyboard_focus_tid = current_task_idx;
    
    // Allocate 4KB for the editor buffer
    char* text_buffer = (char*)kmalloc(4096);
    if (!text_buffer) return;
    kmemset(text_buffer, 0, 4096 / 4);

    // 2. Load existing file if it exists
    struct fat_dir_entry* entry = fat_search(filename);
    uint32_t cursor_pos = 0;
    if (entry) {
        char* loaded_data = fat_load_file(entry);
        if (loaded_data) {
            uint32_t to_copy = (entry->size > 4095) ? 4095 : entry->size;
            kmemcpy(text_buffer, loaded_data, to_copy);
            cursor_pos = to_copy;
            kfree(loaded_data);
        }
    } else {
        // If file doesn't exist, we'll create it on SAVE
        fat_touch(filename);
    }

    VESA_clear();

    while (1) {
        vesa_updating = 1;
        VESA_clear_buffer_only();
        
        // Header
        kprintf_unsync("KED Editor - Editing: %s\n", filename);
        kprintf_unsync("Ctrl+S: Save & Exit | Ctrl+Q: Cancel\n");
        kprintf_unsync("-------------------------------------------\n");
        
        // Render the buffer text
        kprintf_unsync("%s", text_buffer);
        
        // Simple visual cursor (a flashing underscore or pipe)
        if ((system_ticks / 20) % 2 == 0) kprintf_unsync("_");

        vesa_updating = 0;
        VESA_flip();

        // 3. Handle Input
        if (has_key_in_buffer()) {
            char c = get_key_from_buffer();

            if (c == 17) { // Ctrl + Q
                break;
            }
            if (c == 19) { // Ctrl+ S
                fat_write_file(filename, text_buffer);
                break;
            }
            if (c == 16) { 
                uint32_t paste_size = 512;
                // Check if we have room in the 4096 byte buffer
                if (cursor_pos + paste_size < 4094) {
                    for (uint32_t i = 0; i < paste_size; i++) {
                        text_buffer[cursor_pos++] = 'H';
                    }
                    text_buffer[cursor_pos] = '\0'; // Ensure null termination
                }
                continue; // Skip the standard character processing
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
        yield(); // Let the Spinner keep spinning lol!
    }

    kfree(text_buffer);
    keyboard_focus_tid = previous_focus;
    VESA_clear();
}
