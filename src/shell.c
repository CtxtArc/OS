#include "shell.h"
#include "vesa.h"
#include "lib.h"
#include "io.h"
#include "idt.h"
#include "pmm.h"
#include "task.h"
#include "kheap.h"
#include "fat.h"
#include "KED.h"
#include "assembler.h"
#include <stdint.h>
#include "vfs.h"

extern volatile struct task task_list[];
extern int vesa_dirty;
extern int vesa_updating;
extern uint32_t system_ticks;
extern uint32_t total_pages;
extern uint32_t timer_frequency;
extern uint32_t target_fps;
extern int keyboard_focus_tid;
extern int current_task_idx;
char shell_cwd[256] = "/";
     extern void KED_init(const char* filename); 
            extern void KED_task();                     
   

void update_shell_cwd(char* target) {
    if (kstrcasecmp(target, "/") == 0) {
        kstrcpy(shell_cwd, "/");
    } else if (kstrcasecmp(target, "..") == 0) {
        int len = kstrlen(shell_cwd);
        if (len > 1) { // Don't do anything if we are already at "/"
            for (int i = len - 1; i >= 0; i--) {
                if (shell_cwd[i] == '/') {
                    shell_cwd[i] = '\0';
                    if (i == 0) kstrcpy(shell_cwd, "/"); // Return to root
                    break;
                }
            }
        }
    } else {
        if (kstrcmp(shell_cwd, "/") != 0) {
            kstrcat(shell_cwd, "/");
        }
        kstrcat(shell_cwd, target);
    }
}

// Bring in the dirty rectangle helper
extern void mark_task_dirty(int id, int x, int y, int w, int h);

char* find_space(char* str) {
    while (*str) {
        if (*str == ' ') return str;
        str++;
    }
    return 0;
}

void dummy_app() {
    __asm__ volatile("sti");
    while(1) {
        yield();
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

    // --- SYSTEM & INFO ---
    if (kstrcasecmp(input, "HELP") == 0) {
        shell_print("Commands: LS CD CAT MKDIR RM RMDIR PWD TOUCH CLEAR PS KILL SLEEP RUN TOP UPTIME STAT REBOOT CRASH ECHO SET_FPS TIMER GAME HEXDUMP WRITE COMPILE KED\n", COLOR_WHITE);
    }
    else if (kstrcasecmp(input, "SUICIDE") == 0) {
        shell_print("Spawning SUICIDE task... watch the logs.\n", COLOR_WHITE);
        spawn_task(suicide_task,NULL, "suicide_app");
    }
    else if (kstrcasecmp(input, "CAT") == 0) { 
        if (arg) {
            extern vfs_node_t* vfs_walk_path(char* path); 
            vfs_node_t* file = vfs_walk_path(arg);
            
            if (file && (file->flags & FS_FILE)) {
                char* buf = kmalloc(file->size + 1);
                uint32_t bytes_read = vfs_read(file, 0, file->size, (uint8_t*)buf);
                buf[bytes_read] = '\0'; 
                
                shell_print(buf, COLOR_WHITE); 
                shell_print("\n", COLOR_WHITE);
                
                kfree(buf);
            } else {
                shell_print("Error: File not found or is a directory.\n", COLOR_RED);
            }
            if (file) kfree(file); 
        } else {
            shell_print("Usage: CAT <file>\n", COLOR_WHITE);
        }
    }
    else if (kstrcasecmp(input, "LS") == 0) {
        char* target = arg;
        if (target && target[0] == '/') target++; 
        if (target && kstrlen(target) == 0) target = NULL;

        int listing_dev = 0;
        if (target && kstrcasecmp(target, "dev") == 0) {
            listing_dev = 1; 
        } else if (!target && fs_current_dir != fs_root && kstrcasecmp(fs_current_dir->name, "dev") == 0) {
            listing_dev = 1; 
        }

        if (listing_dev) {
            extern int devfs_count;
            extern vfs_node_t devfs_nodes[];
            for (int i = 0; i < devfs_count; i++) {
                shell_print("- ", 0xFFFFFF);
                shell_print(devfs_nodes[i].name, 0xFFFFFF);
                shell_print("\n", 0x555555); 
            }
        } else {
            int listing_root = 0;
            if (!target && fs_current_dir == fs_root) listing_root = 1;
            if (arg && kstrcasecmp(arg, "/") == 0) listing_root = 1; 

            if (listing_root) {
                shell_print("- ", 0x00FFFF); 
                shell_print("dev        ", 0x00FFFF); 
                shell_print(".         VIRTUAL\n", 0x555555);
            }

            if (arg && kstrlen(arg) > 0) {
                uint32_t cluster = fat_get_cluster_from_path(arg); 
                if (cluster != 0xFFFFFFFF) {
                    fat_ls_cluster(cluster); // Note: Make sure fat_ls_cluster also uses shell_print!
                } else {
                    shell_print("Directory not found.\n", COLOR_RED);
                }
            } else {
                fat_ls_cluster(fat_get_current_cluster()); // Note: Make sure fat_ls_cluster uses shell_print!
            }
        }
    } 
    else if (kstrcasecmp(input, "CD") == 0) {
        if (arg) {
            char* target = arg;
            if (target[0] == '/') target++; 
            if (kstrcasecmp(target, "") == 0) target = "/"; 

            if (kstrcasecmp(target, "..") == 0 && kstrcmp(shell_cwd, "/") == 0) {
                return; 
            }

            if (kstrcasecmp(target, "/") == 0 || kstrcasecmp(target, "..") == 0) {
                if (fs_current_dir != fs_root && kstrcasecmp(fs_current_dir->name, "dev") == 0 && kstrcasecmp(target, "..") == 0) {
                    kfree(fs_current_dir);
                    fs_current_dir = fs_root;
                } else {
                    if (fs_current_dir != fs_root) kfree(fs_current_dir);
                    fs_current_dir = fs_root;
                    fat_cd(target); 
                }
                update_shell_cwd(target);
            } else {
                vfs_node_t* next_dir = vfs_finddir(fs_current_dir, target);
                
                if (next_dir && (next_dir->flags & FS_DIRECTORY)) {
                    if (fs_current_dir != fs_root) kfree(fs_current_dir);
                    fs_current_dir = next_dir;
                    
                    if (kstrcasecmp(next_dir->name, "dev") != 0) {
                        fat_cd(target); 
                    }
                    update_shell_cwd(target); 
                } else {
                    shell_print("CD: Directory not found.\n", COLOR_RED);
                    if (next_dir) kfree(next_dir); 
                }
            }
        } else {
            shell_print("Usage: CD <dirname>\n", COLOR_WHITE);
        }
    }
    else if (kstrcasecmp(input, "PS") == 0) {
        shell_print("TID    NAME          STATE\n", COLOR_CYAN);
        for (int i = 0; i < MAX_TASKS; i++) {
            if (task_list[i].state != 0) {
                char buf[16];
                itoa(i, buf, 10);
                shell_print(buf, COLOR_WHITE);
                shell_print("      ", COLOR_WHITE);
                shell_print((char*)task_list[i].name, COLOR_WHITE);
                int len = kstrlen((char*)task_list[i].name);
                for (int j = 0; j < (13 - len); j++) shell_print(" ", COLOR_WHITE);
                if      (task_list[i].state == 1) shell_print("READY\n",   COLOR_GREEN);
                else if (task_list[i].state == 2) shell_print("SLEEP\n",   COLOR_YELLOW);
                else if (task_list[i].state == 3) shell_print("BLOCKED\n", COLOR_RED);
            }
        }
    }
    else if (kstrcasecmp(input, "UPTIME") == 0) {
        char buf[32];
        uint32_t s = system_ticks / 1000; 
        shell_print("Uptime: ", COLOR_WHITE);
        itoa(s, buf, 10);
        shell_print(buf, COLOR_CYAN);
        shell_print("s\n", COLOR_WHITE);
    }
    else if (kstrcasecmp(input, "STAT") == 0) {
        shell_print("--- KERNEL HEAP STATISTICS ---\n", COLOR_CYAN);
        extern header_t* heap_start; 
        #define HEAP_INITIAL_SIZE (64 * 1024 * 1024) 
        
        if (!heap_start) {
            shell_print("Heap is not initialized.\n", COLOR_RED);
        } else {
            uint32_t free_mem = 0; 
            uint32_t used_mem = 0;
            uint32_t blocks = 0;
            
            header_t* curr = heap_start;
            uint32_t heap_limit = (uint32_t)heap_start + HEAP_INITIAL_SIZE; 
            
            while (curr != NULL) {
                if ((uint32_t)curr < (uint32_t)heap_start || (uint32_t)curr >= heap_limit) {
                    shell_print("Error: Heap linked-list corrupted!\n", COLOR_RED);
                    break;
                }
                if (curr->is_free) free_mem += curr->size;
                else used_mem += curr->size;
                blocks++;
                if (curr->size == 0 && curr->next != NULL) {
                    shell_print("Error: Zero-size block detected.\n", COLOR_RED);
                    break;
                }
                curr = curr->next;
            }

            char buf[32];
            shell_print("Total Blocks: ", COLOR_WHITE);
            itoa(blocks, buf, 10);
            shell_print(buf, COLOR_YELLOW);
            shell_print("\n", COLOR_WHITE);

            shell_print("Used Memory:  ", COLOR_WHITE);
            itoa(used_mem / 1024, buf, 10); 
            shell_print(buf, COLOR_YELLOW);
            shell_print(" KB\n", COLOR_WHITE);

            shell_print("Free Memory:  ", COLOR_WHITE);
            itoa(free_mem / 1024, buf, 10); 
            shell_print(buf, COLOR_GREEN);
            shell_print(" KB\n", COLOR_WHITE);
            
            if (free_mem < 4194304) {
                shell_print("WARNING: Low Heap Memory! Close windows or REBOOT.\n", COLOR_RED);
            }
        }
    }
    else if (kstrcasecmp(input, "CLEAR") == 0) {
        // Overwrite the entire window buffer with pure grey
        int sw = task_list[current_task_idx].win_w;
        int sh = task_list[current_task_idx].win_h;
        for(int i = 0; i < sw * sh; i++) {
            task_list[current_task_idx].window_buffer[i] = 0x222222;
        }
        task_list[current_task_idx].cursor_x = 2; // Reset cursor with padding
        task_list[current_task_idx].cursor_y = 2;
    }
    else if (kstrcasecmp(input, "ECHO") == 0) {
        if (arg) { shell_print(arg, COLOR_WHITE); shell_print("\n", COLOR_WHITE); }
    }
    else if (kstrcasecmp(input, "SLEEP") == 0) {
        if (arg) { sleep(katoi(arg)); }
    }
    else if (kstrcasecmp(input, "MKDIR") == 0) {
        if (arg) fat_mkdir(arg);
        else shell_print("Usage: MKDIR <name>\n", COLOR_WHITE);
    }
    else if (kstrcasecmp(input, "RM") == 0) {
        if (arg) fat_rm(arg);
        else shell_print("Usage: RM <filename>\n", COLOR_WHITE);
    }
    else if (kstrcasecmp(input, "RMDIR") == 0) {
        if (arg) fat_rmdir(arg);
        else shell_print("Usage: RMDIR <dirname>\n", COLOR_WHITE);
    }
    else if (kstrcasecmp(input, "PWD") == 0) {
        shell_print(shell_cwd, COLOR_WHITE);
        shell_print("\n", COLOR_WHITE);
    }
    else if (kstrcasecmp(input, "TOUCH") == 0) {
        if (arg) fat_touch(arg);
        else shell_print("Usage: TOUCH <filename>\n", COLOR_WHITE);
    }
    else if (kstrcasecmp(input, "HEXDUMP") == 0) {
        if (arg) fat_hexdump_file(arg); // Needs updating in fat.c too!
        else shell_print("Usage: HEXDUMP <filename>\n", COLOR_RED);
    }
    else if (kstrcasecmp(input, "WRITE") == 0) {
        if (arg && kstrlen(arg) > 0) {
            char* filename = arg;
            char* content = NULL;
            
            for (int i = 0; arg[i] != '\0'; i++) {
                if (arg[i] == ' ') {
                    arg[i] = '\0';
                    content = &arg[i + 1];
                    break;
                }
            }
            
            if (filename && content) {
                struct fat_dir_entry* file = fat_search(filename);
                if (!file) {
                    fat_touch(filename);
                }
                fat_write_file(filename, content);
            }
            else {
                shell_print("Usage: WRITE <file> <text>\n", COLOR_RED);
            }
        }
    }
   
       else if (kstrcasecmp(input, "KED") == 0) {
    if (arg) {
        char* fn = kmalloc(16);
        kstrncpy(fn, arg, 15);
        fn[15] = '\0';
        int tid = spawn_task(KED_task, fn, "KED");  // fn goes into code_ptr
        task_create_window(tid, 0, 0, 0, 0);
        shell_print("Text Editor Spawned.\n", COLOR_GREEN);
    } else {
        shell_print("Usage: KED <filename>\n", COLOR_RED);
    }
}
    else if (kstrcasecmp(input, "TOP") == 0) {
        extern void run_top();
        int tid = spawn_task(run_top, NULL, "TOP");
        task_create_window(tid, 0, 0, 0, 0);
        shell_print("TOP Spawned.\n", COLOR_GREEN);
    }
    else if (kstrcmp(input, "GAME") == 0) {
        extern void task_game();
        int tid = spawn_task(task_game, NULL, "GAME");
        task_create_window(tid, 0, 0, 0, 0);
        shell_print("Game Spawned.\n", COLOR_GREEN);
    }
    else if (kstrcmp(input, "TIMER") == 0) {
        extern void task_timer();
        spawn_task(task_timer, NULL, "TIMER"); 
        shell_print("Background Timer Spawned.\n", COLOR_GREEN);
    }
    // --- EXECUTION ---
    else if (kstrcasecmp(input, "RUN") == 0) {
        if (arg) {
            struct fat_dir_entry* entry = fat_search(arg);
            if (entry) {
                uint32_t size = entry->size;
                char* file_data = fat_load_file(entry);
                void* raw_code = kmalloc(size + 4096);
                
                //kfree(entry); 
                
                if (raw_code) {
                    uint32_t aligned_code = ((uint32_t)raw_code + 0xFFF) & 0xFFFFF000;
                    kmemcpy((void*)aligned_code, file_data, size);
                    kfree(file_data);
                    
                    int tid = spawn_task((void(*)())aligned_code, raw_code, arg);
                    if (tid >= 0) {
                        task_create_window(tid, 0, 0, 0, 0);
                        shell_print("Task Spawned successfully.\n", COLOR_GREEN);
                    } 
                    else if (tid == ERR_TASK_TABLE_FULL) {
                        shell_print("Spawn Error: Task table is full (MAX_TASKS reached).\n", COLOR_RED);
                        kfree(raw_code); 
                    } 
                    else if (tid == ERR_TASK_STACK_OOM) {
                        shell_print("Spawn Error: Not enough memory for task stack.\n", COLOR_RED);
                        kfree(raw_code); 
                    }
                    else {
                        shell_print("Spawn Error: Unknown failure.\n", COLOR_RED);
                        kfree(raw_code); 
                    }
                } else {
                    if (file_data) kfree(file_data); 
                }
            } else shell_print("File not found.\n", COLOR_RED);
        }
    }
    else if (kstrcasecmp(input, "COMPILE") == 0) {
        if (arg) shell_compile(arg);
        else shell_print("Usage: COMPILE <file.txt>\n", COLOR_RED);
    }
    else if (kstrcasecmp(input, "KILL") == 0) {
        if (arg) {
            int id = katoi(arg);
            if (id == 0) shell_print("Cannot kill Shell.\n", COLOR_RED);
            else { kill_task(id); shell_print("Task killed.\n", COLOR_GREEN); }
        }
    }
    else if (input[0] != '\0') {
        shell_print("Unknown command: ", COLOR_RED);
        shell_print(input, COLOR_RED);
        shell_print("\n", COLOR_RED);
    }

    mark_task_dirty(current_task_idx, 0, 0, 4000, 4000);
}

void shell_compile(const char* arg) {
    struct fat_dir_entry* file = fat_search(arg);
    if (!file) {
        kprintf_unsync("Error: %s not found\n", arg);
        return;
    }

    char* source_buf = (char*)fat_load_file(file);
    uint8_t* binary_buf = (uint8_t*)kmalloc(8192);
    
    //kfree(file); // FIX: Free the fat_search entry

    label_count = 0;
    rt_var_count = 0;
    
    for (int pass = 1; pass <= 2; pass++) {
        uint32_t current_pos = 0;
        uint32_t binary_pos = 0;

        while (current_pos < kstrlen(source_buf)) {
            char temp_line[128];
            uint32_t i = 0;
            
            while (current_pos < kstrlen(source_buf) && source_buf[current_pos] != '\n' && i < 127) {
                temp_line[i++] = source_buf[current_pos++];
            }
            temp_line[i] = '\0';
            current_pos++;

            char* walk = temp_line;
            while (*walk == ' ' || *walk == '\t') walk++;
            if (*walk == '#' || *walk == '\0') continue;

            if (pass == 1) {
                assemble_line(temp_line, NULL, &binary_pos, 1);
            } else {
                assemble_line(temp_line, binary_buf, &binary_pos, 2);
            }
        }

        if (pass == 2) {
            uint32_t binary_size = binary_pos;

            char out_name[16];
            int k;
            for (k = 0; k < 11 && arg[k] != '.' && arg[k] != '\0'; k++) out_name[k] = arg[k];
            out_name[k++] = '.'; out_name[k++] = 'B'; out_name[k++] = 'I'; out_name[k++] = 'N'; out_name[k] = '\0';

            fat_touch(out_name);
            fat_write_file_raw(out_name, binary_buf, binary_size);
            kprintf_unsync("Compiled %s to %s (%d bytes)\n", arg, out_name, binary_size);
        }
    }

    kfree(source_buf);
    kfree(binary_buf);
}

void shell_print(char* str, uint32_t color) {
    volatile struct task* t = &task_list[current_task_idx];
    for (int i = 0; str[i] != '\0'; i++) {
        if (str[i] == '\n') {
            t->cursor_x = 0;
            t->cursor_y += 10;
            continue;
        }
        shell_draw_char(str[i], t->cursor_x, t->cursor_y, color, 0x222222);
        t->cursor_x += 8;
        if (t->cursor_x >= t->win_w) {
            t->cursor_x = 0;
            t->cursor_y += 10;
        }
    }
}

// --- NEW HELPER FUNCTIONS: Draw directly to the Window Buffer ---
void shell_draw_char(char c, int x, int y, uint32_t fg, uint32_t bg) {
    volatile struct task* t = &task_list[current_task_idx];
    struct multiboot_info* mbi = VESA_get_boot_info();
    uint32_t sw = mbi->framebuffer_width;

    if (!t->has_window || !t->window_buffer) return;
    
    uint8_t* glyph = font8x8_basic[(unsigned char)c];
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            int dx = x + col;
            int dy = y + row;
            if (dx >= 0 && dx < t->win_w && dy >= 0 && dy < t->win_h) {
                if (glyph[row] & (1 << (7 - col))) t->window_buffer[(dy * sw) + dx] = fg; 
                else t->window_buffer[(dy * sw) + dx] = bg; 
            }
        }
    }
}

