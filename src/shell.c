#include "shell.h"
#include "vesa.h"
#include "lib.h"
#include "io.h"
#include "idt.h"
#include "pmm.h"
#include "task.h"
#include "kheap.h" 
#include "idt.h"
#include "fat.h"
#include "KED.h"
#include "assembler.h"

extern int vesa_updating;
extern uint32_t system_ticks;
extern uint32_t total_pages;
extern uint32_t timer_frequency;        // Add this!
extern uint32_t target_fps;
// Helper to find arguments
char* find_space(char* str) {
    while (*str) {
        if (*str == ' ') return str;
        str++;
    }
    return 0;
}
void dummy_app() {
    // 1. Explicitly re-enable interrupts for this task
    __asm__ volatile("sti"); 

    while(1) {
        // 2. Manually yield so the Shell gets a turn
        yield(); 
        
        // 3. Do a tiny bit of work so we see it's alive
        // (This will flicker a character at the top left)
        volatile char* vga = (char*)0xB8000;
        vga[0]++; 
    }
}

void execute_command(char* input) {
    char* arg = find_space(input);
    if (arg) {
        *arg = '\0'; 
        arg++;       
    }
    int start_y = vesa_cursor_y;
    vesa_updating = 1;
    if (kstrcmp(input, "HELP") == 0) {
        kprintf_unsync("Commands: LS CD CAT MKDIR PWD TOUCH CLEAR STAT PS KILL SLEEP RUN TOP UPTIME REBOOT CRASH ECHO SET_FPS TIMER GAME TEST_MALLOC HEXDUMP WRITE\n");
    }
else if (kstrcmp(input, "CAT") == 0) {
    if (arg) {
        struct fat_dir_entry* file = fat_search(arg);
        if (file && !(file->attr & 0x10)) {
            char* buffer = (char*)fat_load_file(file);
            if (buffer) {
                // Use a loop instead of %s to avoid "runaway" printing
                for (uint32_t i = 0; i < file->size; i++) {
                    // Filter non-printable chars if you want a clean view
                    char c = buffer[i];
                    if (c == '\n' || (c >= 32 && c <= 126)) {
                        kputc(c);
                    } else if (c == '\r') {
                        // Ignore carriage returns or handle them
                    } else {
                        kputc('.'); // Represent binary as dots
                    }
                }
                kputc('\n');
                kfree(buffer);
            }
        } else {
            kprintf_unsync("File '%s' not found or is a directory.\n", arg);
        }
    }
}
else if (kstrcmp(input, "KED") == 0) {
    if (arg) {
        run_editor(arg);
    } else {
        kprintf_unsync("Usage: KED filename.txt\n");
    }
}
else if (kstrcmp(input, "RUN_TEST") == 0) {
    // The raw bytes of the spinner program
    uint8_t test_code[] = {
        0xB8, 0x02, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xC1, 0xE8, 0x05,
        0x83, 0xE0, 0x03, 0xBB, 0x2D, 0x5C, 0x7C, 0x2F, 0x88, 0xC1,
        0xC1, 0xE1, 0x03, 0xD3, 0xEB, 0x81, 0xE3, 0xFF, 0x00, 0x00,
        0x00, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xB9, 0xE8, 0x03, 0x00,
        0x00, 0xBA, 0x05, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xB8, 0x03,
        0x00, 0x00, 0x00, 0xBB, 0x01, 0x00, 0x00, 0x00, 0xCD, 0x80,
        0xEB, 0xC4
    };

    kprintf_color(0xFFFF00, "Starting RUN_TEST (Bypassing FAT)...\n");

    // 1. Allocate raw memory
    uint32_t code_size = sizeof(test_code);
    void* raw_mem = kmalloc(code_size + 8192); // Extra room for alignment
    
    if (!raw_mem) {
        kprintf_color(0xFF0000, "RUN_TEST: kmalloc failed!\n");
        return;
    }

    // 2. Calculate Page Alignment (Same logic as RUN)
    uint32_t raw_addr = (uint32_t)raw_mem;
    uint32_t aligned_exec;
    if ((raw_addr & 0xFFF) == 0) {
        aligned_exec = raw_addr;
    } else {
        aligned_exec = (raw_addr + 0x1000) & 0xFFFFF000;
    }

    kprintf("Allocated: 0x%x, Aligned Entry: 0x%x\n", raw_addr, aligned_exec);

    // 3. Copy hardcoded code to the execution area
    kmemcpy((void*)aligned_exec, test_code, code_size);

    // 4. Spawn the task
    int tid = spawn_task((void(*)())aligned_exec, raw_mem, "TEST_SPIN");

    if (tid != -1) {
        kprintf_color(0x00FF00, "Task spawned successfully. TID: %d\n", tid);
    } else {
        kprintf_color(0xFF0000, "spawn_task failed!\n");
    }
}
else if (kstrcmp(input, "MKDIR") == 0) {
    if (arg) {
        fat_mkdir(arg);
    } else {
        kprintf_unsync("Usage: MKDIR <name>\n");
    }
}

else if (kstrcmp(input, "SET_FPS") == 0) {
    if (arg) {
        target_fps = katoi(arg);
        kprintf_unsync("FPS set to %d\n", target_fps);
    }
}
    else if (kstrcmp(input, "TIMER") == 0){
    // We pass 0 for 'raw_code' because it's a kernel-space function, not an loaded file.
    int tid = spawn_task(task_timer, 0, "timer");
    kprintf_unsync("Timer task spawned (TID: %d)\n", tid);
    }
     else if (kstrcmp(input, "GAME") == 0){
    // We pass 0 for 'raw_code' because it's a kernel-space function, not an loaded file.
    task_game();
    }
    else if (kstrcmp(input, "ECHO") == 0) {
        if (arg) kprintf_unsync("%s\n", arg);
        else kprintf_unsync("Usage: ECHO [text]\n");
    }
    else if (kstrcmp(input, "STAT") == 0) {
        // Assuming kheap_stats now uses unsync internal prints
        kheap_stats();  
    }
    else if (kstrcmp(input, "SLEEP") == 0) {
        if (arg) {
            int ms = katoi(arg);
            if (ms > 0) {
                kprintf_unsync("Shell sleeping for %d ms...\n", ms);
                VESA_flip(); // Flip here because we are about to block/sleep
                sleep(ms); 
                kprintf_unsync("Shell awake.\n");
            }
        } else {
            kprintf_unsync("Usage: SLEEP [ms]\n");
        }
    }



else if (kstrcmp(input, "RUN") == 0) {
        if (arg) {
              struct fat_dir_entry* entry = fat_search(arg);
                uint32_t size = entry->size;
                char* file_data = fat_load_file(entry);
                void* raw_code = kmalloc(size + 4096);
                if (!raw_code) {
                    kprintf_unsync("Memory allocation failed\n");
                } else {
                    uint32_t aligned_code = ((uint32_t)raw_code + 0xFFF) & 0xFFFFF000;
                    kmemcpy((void*)aligned_code, file_data, size);
                    kfree(file_data); 
                    int tid = spawn_task((void(*)())aligned_code, raw_code, arg);
                    kprintf_unsync("Spawned %s (TID: %d, Entry: 0x%x)\n", arg, tid, aligned_code);
                }
        }
}

  else if (kstrcmp(input, "TEST_MALLOC") == 0) {
    
    kprintf_unsync("\n--- STARTING FAT-MIMIC HEAP TEST ---\n");
    kheap_stats();

    // 1. Allocate two blocks exactly like mkdir does
    uint8_t* bufA = (uint8_t*)kmalloc(512);
    uint8_t* bufB = (uint8_t*)kmalloc(512);

    kprintf_unsync("Allocated A at: 0x%x, B at: 0x%x\n", (uint32_t)bufA, (uint32_t)bufB);
    kheap_stats(); // Should show 2 more blocks used

    // 2. THE SMASH TEST: Fill Buf A with exactly 512 bytes
    // We use 0xAA so we can see it if it leaks into the header of B
    kmemset(bufA, 0xAA, 512 / 4); 

    // 3. CHECK INTEGRITY
    // If kmemset/overflow hit the header of B, B's metadata will be garbage.
    header_t* headerB = (header_t*)((uint32_t)bufB - sizeof(header_t));
    
    if (headerB->size != 512) {
        kprintf_unsync("!!! BUG DETECTED !!! Header B size corrupted! Expected 512, got %d\n", headerB->size);
    }
    if (headerB->is_free != 0) {
        kprintf_unsync("!!! BUG DETECTED !!! Header B marked as FREE while in use!\n");
    }

    // 4. THE FRAGMENTATION TEST
    kprintf_unsync("Freeing A and B...\n");
    kfree(bufA);
    kfree(bufB);
    kheap_stats(); // Should return to original state

    // 5. THE "TAIL EATER" TEST
    // Allocate something small, then something huge.
    void* small = kmalloc(16);
    void* big = kmalloc(1024 * 1024); // 1MB
    
    if (big == NULL) {
        kprintf_unsync("!!! BUG DETECTED !!! Small allocation 'ate' the rest of the heap.\n");
    }

    kfree(small);
    kfree(big);
    kprintf_unsync("--- TEST COMPLETE ---\n");
    kheap_stats();
}

  
    else if (kstrcmp(input, "HEXDUMP") == 0) {
    if (arg) {
        fat_hexdump_file(arg);
    } else {
        kprintf_unsync("Usage: HEXDUMP <filename>\n");
    }
}  
else if (kstrcmp(input, "RM") == 0) {
    if (arg) fat_rm(arg);
    else kprintf_unsync("Usage: RM <filename>\n");
}
else if (kstrcmp(input, "RMDIR") == 0) {
    if (arg) fat_rmdir(arg);
    else kprintf_unsync("Usage: RMDIR <dirname>\n");
}

else if (kstrcmp(input, "LS") == 0) {
    if (arg && kstrlen(arg) > 0) {
        uint32_t target = fat_get_cluster_from_path(arg);
        if (target != 0xFFFFFFFF) {
            fat_ls_cluster(target);
        } else {
            kprintf_unsync("Directory not found.\n");
        }
    } else {
        fat_ls_cluster(fat_get_current_cluster());
      
    }
}

else if (kstrcmp(input, "CD") == 0) {
    if (arg) {
        fat_cd(arg);
    } else {
        kprintf_unsync("Usage: CD <dirname>\n");
    }
}
else if (kstrcmp(input, "TOUCH") == 0) {
    if (arg) {
        fat_touch(arg);
    } else {
        kprintf_unsync("Usage: TOUCH <filename>\n");
    }
}

else if (kstrcmp(input, "PWD") == 0) {
    if (fat_get_current_cluster() == 0) {
        kprintf_unsync("/\n");
    } else {
        fat_print_path_recursive(fat_get_current_cluster());
        kprintf_unsync("\n");
    }
}
   else if (kstrcmp(input, "WRITE") == 0) {
    // 'arg' contains everything after "WRITE " (e.g., "test.txt hello world")
    if (arg && kstrlen(arg) > 0) {
        char* filename = arg;
        char* content = NULL;

        // 1. Find the space that separates the filename from the text
        for (int i = 0; arg[i] != '\0'; i++) {
            if (arg[i] == ' ') {
                arg[i] = '\0';        // Null-terminate the filename here
                content = &arg[i + 1]; // Content starts at the next character
                break;
            }
        }

        // 2. Validate that we have both a name and something to write
        if (filename && content && kstrlen(content) > 0) {
            fat_write_file(filename, content);
        } else {
            kprintf_unsync("Usage: WRITE <filename> <text content>\n");
        }
    } else {
        kprintf_unsync("Usage: WRITE <filename> <text content>\n");
    }
} 
    else if (kstrcmp(input, "PS") == 0) {
        kprintf_unsync("TID   NAME         STATE\n");
        for (int i = 0; i < MAX_TASKS; i++) {
            if (task_is_ready(i)) {
                char* name = task_get_name(i);
                kprintf_unsync("%d     %s", i, name);
                int len = kstrlen(name);
                for (int j = 0; j < (12 - len); j++) kprintf_unsync(" ");
                
                if (task_get_state(i) == 1)      kprintf_unsync(" READY\n");
                else if (task_get_state(i) == 2) kprintf_unsync(" SLEEP\n");
            }
        }
    }
    else if (kstrcmp(input, "TOP") == 0) {
        run_top(); 
        VESA_clear(); // Clear back to shell after exiting TOP
    }
    else if (kstrcmp(input, "UPTIME") == 0) {
        uint32_t current_ticks = system_ticks;
        uint32_t current_freq = 100; // Assuming 100Hz
        uint32_t s = current_ticks / current_freq;
        kprintf_unsync("Uptime: %ds (Ticks: %d)\n", s, current_ticks);
    }
    else if (kstrcmp(input, "KILL") == 0) {
        if (arg) {
            int id = arg[0] - '0';
            if (id == 0) kprintf_unsync("Error: Cannot kill Shell\n");
            else {
                kill_task(id);
                kprintf_unsync("Task %d killed.\n", id);
            }
        }
    }
    else if (kstrcmp(input, "CLEAR") == 0) {
        VESA_clear_buffer_only(); 
        // No need to flip here, the final flip handles it
    }
    else if (kstrcmp(input, "REBOOT") == 0) {
        kprintf_unsync("Rebooting...\n");
        VESA_flip(); // Must flip so user sees message before CPU resets
        outb(0x64, 0xFE);
    }
    else if (kstrcmp(input, "CRASH") == 0) {
        kprintf_unsync("Triggering CPU Exception...\n");
        VESA_flip();
        __asm__ volatile("div %0" :: "r"(0));
    }

  else if (kstrcmp(input, "COMPILE") == 0) {
    if (arg[0] != '\0') {
        shell_compile(arg);
    } else {
        kprintf_unsync("Usage: compile <filename.txt>\n");
    }
}

    else if (input[0] != '\0') {
        kprintf_unsync("Unknown command: %s\n", input);
    }
   
    int lines_touched = (vesa_cursor_y - start_y) + 12;
    vesa_updating = 0; 
    struct multiboot_info* boot_info = VESA_get_boot_info();
    if (lines_touched > 0 && lines_touched < (int)boot_info->framebuffer_height) {
        VESA_flip_rows(start_y, lines_touched);
    } else {
        // If we scrolled, the whole screen changed, just do a full flip
        VESA_flip();
    }
}
void shell_compile(const char* arg) {
    struct fat_dir_entry* file = fat_search(arg);
    if (!file) {
        kprintf_unsync("Error: %s not found\n", arg);
        return;
    }

    // 1. Load the ASCII text from disk
    char* source_buf = (char*)fat_load_file(file);
    
    // 2. Allocate a buffer for the ACTUAL MACHINE CODE (binary)
    uint8_t* binary_buf = (uint8_t*)kmalloc(4096);
    uint32_t binary_size = 0; // This tracks ACTUAL BYTES generated

    char* line = source_buf;
    uint32_t current_pos = 0;

    while (current_pos < file->size) {
        char temp_line[128];
        uint32_t i = 0;
        
        // Extract one line from the source_buf
        while (current_pos < file->size && line[i] != '\n' && i < 127) {
            temp_line[i] = line[i];
            i++;
        }
        temp_line[i] = '\0';
        
        // Move our source pointers forward
        current_pos += (i + 1);
        line += (i + 1);

        // --- THE MAGIC STEP ---
        // assemble_line must write hex bytes into binary_buf at offset 'binary_size'
        assemble_line(temp_line, binary_buf, &binary_size);
    }


    // 4. WRITE THE BINARY BUFFER, NOT THE SOURCE BUFFER
    // 4. Generate output filename (e.g., TEST.TXT -> TEST.BIN)
    char out_name[16];
    int i = 0;
    for (i = 0; i < 11 && arg[i] != '.' && arg[i] != '\0'; i++) {
        out_name[i] = arg[i];
    }
    out_name[i++] = '.';
    out_name[i++] = 'B';
    out_name[i++] = 'I';
    out_name[i++] = 'N';
    out_name[i] = '\0';

    // 5. Save the output
    fat_touch(out_name);
    fat_write_file_raw(out_name, binary_buf, binary_size);

    kprintf_unsync("Compiled: %s (%d instructions/bytes)\n", out_name, binary_size);

    // 6. Final Cleanup
    kfree(source_buf);
    kfree(binary_buf);
}
