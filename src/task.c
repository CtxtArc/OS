#include "task.h"
#include "vesa.h"
#include <stdint.h>
#include "kheap.h"
#include "io.h"
#include "shell.h"
#include "lib.h"

int keyboard_focus_tid = 0; // Default focus is the Shell (Task 0)
extern int vesa_updating;
// External assembly function
extern void switch_to_stack(uint32_t* old_esp, uint32_t new_esp);
extern volatile uint32_t system_ticks;
int multitasking_enabled = 0;
// The Task Control Block (TCB) array
volatile struct task task_list[MAX_TASKS];
int current_task_idx = 0;
void shell_task() {
    char line[128];
    int idx = 0;

    VESA_print("KDXOS Shell Started.\n", COLOR_CYAN);
    VESA_print("> ", COLOR_YELLOW);

    while(1) {
        char c = keyboard_getchar(); // This calls yield() internally if no key

        if (c == '\n') {
            line[idx] = '\0';
            VESA_print("\n", COLOR_WHITE);
            if (idx > 0) {
                execute_command(line); 
            }
            //vesa_updating = 0; 
            idx = 0;
            VESA_print("> ", COLOR_YELLOW);
        } 
        else if (c == '\b' && idx > 0) {
            // backspace and non empty line
            idx--;
            if (vesa_cursor_x >= 8) {
              vesa_cursor_x -= 8;
            }
            VESA_draw_char(' ', vesa_cursor_x, vesa_cursor_y, 0x222222); 
        } 
        else if (idx < 127 && c >= ' ') {
            line[idx++] = c;
            char str[2] = {c, '\0'};
            VESA_print(str, COLOR_WHITE);
            vesa_updating = 1; // LOCK THE SCREEN
            VESA_flip();
            vesa_updating = 0; // UNLOCK
        }
    }
}

int spawn_task(void (*entry_point)(), void* code_ptr, char* name) {
    for (int i = 1; i < MAX_TASKS; i++) {
        if (task_list[i].state == 0) {
            // 1. Reset Metadata & Set Name
            task_list[i].has_drawn = 0;
            task_list[i].last_x = 0;
            task_list[i].last_y = 0;

            // Initialize CPU Accounting and Sleep state ---
            task_list[i].total_ticks = 0;    // Reset CPU odometer
            task_list[i].sleep_ticks = 0;    // Ensure it doesn't start asleep
      
            // Copy name safely
            kstrncpy((char*)task_list[i].name, name, 15);
            task_list[i].name[15] = '\0'; 

            // 2. Allocate RAW memory (Store this for kfree)
            // We allocate 8KB to ensure we can find a 4KB aligned block inside
            void* raw_stack = kmalloc(4096 + 0x1000);
            if (!raw_stack) return -1;

            task_list[i].stack_ptr = raw_stack; 
            task_list[i].code_ptr = code_ptr; 

            // 3. Calculate the ALIGNED stack for the CPU
            uint32_t aligned_stack = (uint32_t)raw_stack;
            if (aligned_stack & 0xFFF) {
                aligned_stack &= 0xFFFFF000;
                aligned_stack += 0x1000;
            }

            // 4. Build the stack frame at the TOP of the aligned 4KB
            uint32_t* s_ptr = (uint32_t*)(aligned_stack + 4096);

            // --- THE IRET FRAME ---
            *--s_ptr = 0x10;                     // SS
            uint32_t task_stack_top = (uint32_t)s_ptr; 
            *--s_ptr = task_stack_top;           // ESP
            *--s_ptr = 0x202;                    // EFLAGS
            *--s_ptr = 0x08;                     // CS
            *--s_ptr = (uint32_t)entry_point;    // EIP

            // --- INT DATA (Matches irq0_handler) ---
            *--s_ptr = 0;                        // err_code
            *--s_ptr = 32;                       // int_no (Timer)

            // --- PUSHA FRAME ---
            for(int j = 0; j < 8; j++) *--s_ptr = 0;

            // --- DATA SEGMENT ---
            *--s_ptr = 0x10;                     // DS

            // 5. Save final ESP and set to READY
            task_list[i].esp = (uint32_t)s_ptr;
            task_list[i].state = 1; 
            
            return i;
        }
    }
    return -1;
}

// Stacks for the tasks (10 stacks of 4KB each)
uint32_t task_stacks[MAX_TASKS][1024];

// Correct yield loop logic
void yield() {
    __asm__ volatile("hlt"); // Trigger the Timer Interrupt manually
}

void kill_task(int id) {
    if (id <= 0 || id >= MAX_TASKS) return;

    if (task_list[id].has_drawn) {
        int w = task_list[id].last_x - task_list[id].first_x;
        int h = task_list[id].last_y - task_list[id].first_y;
        
        // Safety check to prevent massive unsigned underflow clears
        if (w > 0 && w < 2000 && h > 0 && h < 2000) {
            VESA_clear_region(task_list[id].first_x, task_list[id].first_y, w, h);
            VESA_flip(); 
        }
    }

    // Mark as dead immediately to stop the scheduler from picking it
    task_list[id].state = 0;

    if (task_list[id].stack_ptr) {
        kfree(task_list[id].stack_ptr);
        task_list[id].stack_ptr = NULL;
    }
    if (task_list[id].code_ptr) {
        kfree(task_list[id].code_ptr);
        task_list[id].code_ptr = NULL;
    }

    // Reset everything for the next spawn
    task_list[id].has_drawn = 0;
    task_list[id].first_x = 0;
    task_list[id].first_y = 0;
    task_list[id].last_x = 0;
    task_list[id].last_y = 0;
}

void idle_task_code() {
    while(1) {
        __asm__ volatile("hlt");
    }
}

void init_multitasking() {
    multitasking_enabled = 1;
    for (int i = 0; i < MAX_TASKS; i++) {
        // Zero everything!
        //kmemset((void*)&task_list[i], 0, sizeof(struct task));
        
        task_list[i].state = 0; // DEAD
        task_list[i].kbd_head = 0;
        task_list[i].kbd_tail = 0;
    }
    
    // Task 0: Shell
    task_list[0].state = 1; // READY
    kstrncpy((char*)task_list[0].name, "shell", 15);
    keyboard_focus_tid = 0; // Ensure focus starts on shell
    
    // Task 1: Idle Task (Always READY)
    spawn_task(idle_task_code, NULL, "idle");
    
    current_task_idx = 0;
}
// Helper function
int get_current_task_id() {
    return current_task_idx;
}

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

int task_get_state(int id){
    if (id < 0 || id >= MAX_TASKS) return -1;
    return task_list[id].state;
}
int task_get_sleep_ticks(int id){
  if (id < 0 || id >= MAX_TASKS) return -1;
  return task_list[id].sleep_ticks;
}
int task_get_total_ticks(int id){
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
    
  // 1. Preparation
    while (has_key_in_buffer()) { get_key_from_buffer(); }
    
    vesa_updating = 1;      // Lock out background flips
    VESA_clear();           // Full screen clear for "App Mode"
    vesa_updating = 0;

    int x = 500, y = 500;
    int old_x = 500, old_y = 500;
    int exit = 0;

    // 2. Initial Draw
    VESA_draw_char('*', x, y, 0x00FFFF);
    VESA_flip();

    while (exit == 0) {
        if (has_key_in_buffer()) {
            char c = get_key_from_buffer();
            
            if (c == 'q' || c == 'Q') {
                exit = 1;
                break;
            }

            // Save old coordinates to erase them
            old_x = x;
            old_y = y;

            switch (c) {  
                case 'w': y -= 8; break;
                case 's': y += 8; break;
                case 'a': x -= 8; break;
                case 'd': x += 8; break;
            }

            // 3. Optimized Rendering
            vesa_updating = 1;
            
            // Wipe ONLY the 8x8 pixels of the previous position
            VESA_clear_region(old_x, old_y, 8, 8);
            
            // Draw new position
            VESA_draw_char('*', x, y, 0x00FFFF);

            // TIGHTEN the metadata box so 'KILL' only wipes the current player
            task_list[current_task_idx].first_x = x;
            task_list[current_task_idx].first_y = y;
            task_list[current_task_idx].last_x = x + 8;
            task_list[current_task_idx].last_y = y + 8;

            vesa_updating = 0;
            VESA_flip(); 
        } else {
            // Stay polite! Let the spinner and shell logic breathe
            yield();
        }
    }

    // 4. Exit Cleanup
    vesa_updating = 1;
    VESA_clear(); // Clear the game screen before returning to shell
    vesa_updating = 0;
    keyboard_focus_tid = previous_focus;
    // Reset Shell cursor to top-left so the prompt looks right
    vesa_cursor_x = 0;
    vesa_cursor_y = 0;
}
void run_top() {
int previous_focus = keyboard_focus_tid;
    keyboard_focus_tid = current_task_idx;

// 1. CLEAR the keyboard buffer so we don't process old keys
    while (has_key_in_buffer()) {
        get_key_from_buffer();
    }
    // Initial clear
    VESA_clear();
    
    while (1) {
        // Move cursor to 0,0 or clear
          vesa_updating = 1;         // Lock the timer out
        VESA_clear_buffer_only();


        kprintf_unsync("KDXOS TOP - System Ticks: %d\n", system_ticks);
        kprintf_unsync("Press 'q' to return to Shell\n");
        kprintf_unsync("-------------------------------------------\n");
        kprintf_unsync("TID   NAME         STATE      CPU-TICKS\n");

        for (int i = 0; i < MAX_TASKS; i++) {
            if (task_get_state(i) != 0) {
                // Print TID and Name
                kprintf_unsync("%d     %s", i, task_get_name(i));

                // Manual Padding for Name Column (since no %-13s)
                int name_len = kstrlen(task_get_name(i));
                for (int j = 0; j < (13 - name_len); j++) kprintf_unsync(" ");

                // Print State
                if (task_get_state(i) == 1)      kprintf_unsync("READY      ");
                else if (task_get_state(i) == 2) kprintf_unsync("SLEEP      ");

                // Print Ticks (We added this field to the task struct earlier)
                kprintf_unsync("%d\n", task_get_total_ticks(i));
            }
        }
        vesa_updating = 0;         // Unlock
        VESA_flip();               // Show finished frame


        // --- NON-BLOCKING CHECK ---
        // We use your io.c functions here
        if (has_key_in_buffer()) {
            char c = get_key_from_buffer(); 
            if (c == 'q' || c == 'Q') {
                break; // Exit the loop
            }
        }
               sleep(500); 
    }
    
    VESA_clear_buffer_only();

    keyboard_focus_tid = previous_focus;
    kprintf_unsync("Returned to Shell.\n");
  VESA_flip();
}
