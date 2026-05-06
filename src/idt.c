#include "idt.h"
#include "io.h"
#include "lib.h"
#include "task.h"
#include "vesa.h"
#include "kheap.h"
#include "assembler.h"

uint32_t timer_frequency = 0; // Global variable to store the frequency
extern struct task task_list[];
extern int current_task_idx;
extern void isr0(); // Declaration of the assembly label
volatile uint32_t system_ticks = 0;
struct idt_entry idt[256];
struct idt_ptr idtp;
extern void isr128_stub();
extern int keyboard_focus_tid;
extern int vesa_updating;
extern void idle_task_code();

char *exception_messages[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Into Detected Overflow",
    "Out of Bounds",
    "Invalid Opcode",
    "No Coprocessor",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Bad TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Unknown Interrupt",
};


// Ensure 'struct registers' is defined in idt.h exactly 
// as the stack was pushed in assembly!
void isr_handler(struct registers *r) {
    VESA_clear();
    for(int i = 0; i < 80 * 25; i++) {
        ((uint16_t*)0xB8000)[i] = (uint16_t)' ' | (uint16_t)0x1F << 8;
    }
    vesa_cursor_x = 0;
    vesa_cursor_y = 0;


    kprintf_color(COLOR_WHITE, "--- KERNEL PANIC ---\n");
    kprintf_color(COLOR_WHITE, "Interrupt: %d (%s)\n", r->int_no, exception_messages[r->int_no]);
    kprintf_color(COLOR_WHITE, "EIP: %x  EAX: %x  EBX: %x\n", r->eip, r->eax, r->ebx);
    kprintf_color(COLOR_WHITE, "ECX: %x  EDX: %x\n", r->ecx, r->edx);
    
    while(1) __asm__("hlt");
}

// In idt_init, map the first entry

void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low = (base & 0xFFFF);
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].sel = sel;
    idt[num].always0 = 0;
    idt[num].flags = flags;
}


extern void irq1_handler(); // The assembly function

extern char buffer[128];
extern int buffer_idx;
extern int shift_state;
extern int ctrl_state;

char kbd_buffer[KEYBOARD_BUFFER_SIZE];
int kbd_head = 0;
int kbd_tail = 0;


int ctrl_state = 0;
int shift_state = 0;
int alt_state = 0;

void pic_remap() {
    outb(0x20, 0x11); // Initialize Master PIC
    outb(0xA0, 0x11); // Initialize Slave PIC
    outb(0x21, 0x20); // Map Master to 0x20 (32)
    outb(0xA1, 0x28); // Map Slave to 0x28 (40)
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0xFC);
    outb(0xA1, 0xFF); // Mask all Slave IRQs
}



void idt_init() {
    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base = (uint32_t)&idt;
    // Clear the table
    for(int i = 0; i < 256; i++) {
        idt_set_gate(i, 0, 0, 0);
    }

    idt_set_gate(128, (uint32_t)isr128_stub, 0x08, 0x8E);
    //  Handle the Timer (IRQ 0 -> INT 32)
    extern void irq0_handler();
    idt_set_gate(32, (uint32_t)irq0_handler, 0x08, 0x8E);
    idt_set_gate(0, (uint32_t)isr0, 0x08, 0x8E);
    extern void isr13();
    idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8E); // Register GPF handler

    // Keep your Keyboard (IRQ 1 -> INT 33)
    extern void irq1_handler();
    idt_set_gate(33, (uint32_t)irq1_handler, 0x08, 0x8E);

    __asm__ volatile("lidt (%0)" : : "r" (&idtp));
}





void timer_init(uint32_t frequency) {
    if (frequency == 0) frequency = 1; // Prevent division by zero
    timer_frequency = frequency; 

    uint32_t divisor = 1193182 / frequency;
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

uint32_t next_stack_ptr = 0;
extern uint32_t target_fps;

void timer_handler(struct registers *regs) {
    system_ticks++;

    // 1. Update Video Sync
    uint32_t ticks_per_frame = timer_frequency / target_fps;
    if (ticks_per_frame == 0) ticks_per_frame = 1;

    if (system_ticks % ticks_per_frame == 0) {
        if (!vesa_updating) {
            VESA_flip();
        }
    }

    // 2. Multitasking Logic
    if (multitasking_enabled) {
        // CPU Accounting: Only count if the task is alive
        if (task_list[current_task_idx].state != 0) {
            task_list[current_task_idx].total_ticks++;
        }

        // --- Handle Sleep States ---
        for (int i = 0; i < MAX_TASKS; i++) {
            if (task_list[i].state == 2) { // SLEEPING
                if (task_list[i].sleep_ticks > 0) {
                    task_list[i].sleep_ticks--;
                }
                if (task_list[i].sleep_ticks == 0) {
                    task_list[i].state = 1; // Wake up!
                }
            }
        }

        // --- THE SCHEDULER ---
        
        // Save the stack pointer of the task we are leaving
        task_list[current_task_idx].esp = (uint32_t)regs;

        // Search for the next READY task
        int next_task = (current_task_idx + 1) % MAX_TASKS;
        int found = -1;

        for (int i = 0; i < MAX_TASKS; i++) {
            int idx = (next_task + i) % MAX_TASKS;
            
            // CRITICAL: Only pick tasks in the READY state (1)
            // This prevents jumping into tasks that are SLEEPING (2), 
            // BLOCKED (3), or DEAD (0).
            if (task_list[idx].state == 1) {
                found = idx;
                break;
            }
        }

        // Safety: If no tasks are ready (shouldn't happen with an Idle task),
        // stay on the current task if it's still alive, otherwise pick task 0.
        if (found == -1) {
            if (task_list[current_task_idx].state == 1) found = current_task_idx;
            else found = 0; 
        }

        // Perform the switch
        current_task_idx = found;
        next_stack_ptr = task_list[current_task_idx].esp;
    }

    // 3. Send EOI to PIC
    outb(0x20, 0x20);
}


// Track shift state globally in idt.c or io.c

void keyboard_handler(struct registers *regs) {
    (void)regs; 
    uint8_t scancode = inb(0x60);

    // 1. Update modifier states
    if (scancode == 0x1D)      { ctrl_state = 1; } 
    else if (scancode == 0x9D) { ctrl_state = 0; } 
    else if (scancode == 0x2A || scancode == 0x36) { shift_state = 1; } 
    else if (scancode == 0xAA || scancode == 0xB6) { shift_state = 0; } 
    
    // Emergency Rescue: F1 always returns focus to the Shell
    else if (scancode == 0x3B) { 
        keyboard_focus_tid = 0; 
    }

    // 2. Process Key Press (Ignore break codes/key releases)
    else if (!(scancode & 0x80)) {
        char c = scancode_to_ascii(scancode, shift_state);
        
        if (c != 0) {
            // Find the task that currently owns the keyboard
            volatile struct task *target = &task_list[keyboard_focus_tid];

            // Calculate buffer position (using a power-of-two size like 64 is efficient)
            int next = (target->kbd_head + 1) % 64; 

            if (next != target->kbd_tail) {
                target->kbd_buffer[target->kbd_head] = c;
                target->kbd_head = next;
                
                // --- THE WAKEUP CALL ---
                // If the task was blocked waiting for input, mark it READY.
                // The scheduler (timer_handler) will now see it and resume it.
                if (target->state == 3) { 
                    target->state = 1; 
                }
            }
        }
    }

    outb(0x20, 0x20); // End of Interrupt
}

void emit_mov(uint8_t reg_code, uint32_t val, uint8_t* out_buf, uint32_t* pos) {
    out_buf[(*pos)++] = reg_code;
    kmemcpy(&out_buf[*pos], &val, 4);
    *pos += 4;
}
void emit_load(uint8_t reg_opcode, const char* arg, uint8_t* out_buf, uint32_t* pos) {
    // If it's a number (literal)
    if ((arg[0] >= '0' && arg[0] <= '9') || arg[0] == '-') {
        uint32_t val = (arg[0] == '0' && arg[1] == 'x') ? katoh(arg) : katoi(arg);
        out_buf[(*pos)++] = reg_opcode; // e.g., 0xB8 for EAX, 0xBB for EBX
        kmemcpy(&out_buf[*pos], &val, 4);
        *pos += 4;
    } 
    // If it's a variable (memory address)
    else {
        uint32_t offset = get_var_offset(arg);
        // We use the Opcode 0x8B (MOV reg, [mem])
        // This is slightly more complex x86 encoding:
        out_buf[(*pos)++] = 0x8B;
        
        // This "ModR/M" byte selects which register to load into
        if (reg_opcode == 0xB8) out_buf[(*pos)++] = 0x05; // EAX
        else if (reg_opcode == 0xBB) out_buf[(*pos)++] = 0x1D; // EBX
        else if (reg_opcode == 0xB9) out_buf[(*pos)++] = 0x0D; // ECX
        else if (reg_opcode == 0xBA) out_buf[(*pos)++] = 0x15; // EDX
        else if (reg_opcode == 0xBE) out_buf[(*pos)++] = 0x35; // ESI
        else if (reg_opcode == 0xBF) out_buf[(*pos)++] = 0x3D; // EDI
        
        kmemcpy(&out_buf[*pos], &offset, 4);
        *pos += 4;
    }
}


void syscall_handler(struct registers *regs) {
// IMMEDIATELY re-enable interrupts so the keyboard and timer aren't starved!
    __asm__ volatile("sti");
    if (regs->eax == 1) { // Syscall 1: DRAW_CHAR
        char c = (char)regs->ebx;
        int x = regs->ecx;
        int y = regs->edx;
        int id = current_task_idx;

        if (task_list[id].has_window) {
            // --- WINDOW MANAGER MODE ---
            extern uint8_t font8x8_basic[128][8]; // Import your font
            uint8_t* glyph = font8x8_basic[(int)c];
            uint32_t text_color = 0xFFFFFF; // White text
            uint32_t bg_color = 0x222222;   // Dark grey background

            for (int row = 0; row < 8; row++) {
                uint8_t data = glyph[row];
                for (int col = 0; col < 8; col++) {
                    int draw_x = x + col;
                    int draw_y = y + row;

                    // Clip to window bounds
                    if (draw_x >= 0 && draw_x < task_list[id].win_w && 
                        draw_y >= 0 && draw_y < task_list[id].win_h) {
                        
                        int pixel_index = (draw_y * task_list[id].win_w) + draw_x;
                        
                        if (data & (1 << (7 - col))) {
                            task_list[id].window_buffer[pixel_index] = text_color;
                        } else {
                            // Optional: Comment this out if you want transparent text backgrounds
                            task_list[id].window_buffer[pixel_index] = bg_color; 
                        }
                    }
                }
            }
            task_list[id].has_drawn = 1;
            
        } else {
            // --- LEGACY CLI MODE ---
            if (x < task_list[id].first_x) task_list[id].first_x = x;
            if (y < task_list[id].first_y) task_list[id].first_y = y;
            if (x + 8 > task_list[id].last_x) task_list[id].last_x = x + 8;
            if (y + 8 > task_list[id].last_y) task_list[id].last_y = y + 8;

            task_list[id].has_drawn = 1;
            VESA_draw_char(c, x, y, 0xFFFFFF); 
        }
    }    else if (regs->eax == 2) { // Get Ticks
        regs->eax = system_ticks; 
    }


else if (regs->eax == 3) { // Syscall 3: Sleep(ms)
        uint32_t ms = regs->ebx;
        
        uint32_t ticks_to_sleep = (ms * timer_frequency) / 1000;
        if (ms > 0 && ticks_to_sleep == 0) {
            ticks_to_sleep = 1;
        }

        task_list[current_task_idx].sleep_ticks = ticks_to_sleep; 
        task_list[current_task_idx].state = 2; // Set state to SLEEPING
        
        // DO NOT change current_task_idx manually!
        // We just return. The task will execute a few more instructions 
        // until the Timer Interrupt (IRQ0) fires safely and puts it to sleep.
    }
   
  else if (regs->eax == 4) { // Syscall 4: Exit/Terminate
        kprintf_unsync("Task %d exited.\n", current_task_idx);
        task_list[current_task_idx].state = 0; // Set to DEAD
        
        if (task_list[current_task_idx].has_drawn) {
            int w = task_list[current_task_idx].last_x - task_list[current_task_idx].first_x;
            int h = task_list[current_task_idx].last_y - task_list[current_task_idx].first_y;
            
            if (w > 0 && w < 2000 && h > 0 && h < 2000) {
                VESA_clear_region(task_list[current_task_idx].first_x, task_list[current_task_idx].first_y, w, h);
                VESA_flip(); 
            }
        }
        
        // --- CLEANUP ---
        if (task_list[current_task_idx].stack_ptr != NULL) {
            kfree(task_list[current_task_idx].stack_ptr);
            task_list[current_task_idx].stack_ptr = NULL;
        }
        if (task_list[current_task_idx].code_ptr != NULL) {
            kfree(task_list[current_task_idx].code_ptr);
            task_list[current_task_idx].code_ptr = NULL; 
        }
        if (task_list[current_task_idx].window_buffer != NULL) {
            kfree(task_list[current_task_idx].window_buffer);
            task_list[current_task_idx].window_buffer = NULL; 
        }

        // DO NOT change current_task_idx manually!
        // Instead, force the CPU's Instruction Pointer to jump into the Idle loop.
        // It will safely 'hlt' here until the Timer sweeps it away.
        regs->eip = (uint32_t)idle_task_code;
    }else if (regs->eax == 5) { // Syscall 5: Clear Screen
    VESA_clear();
    // It's good practice to also reset the kernel's bounding box 
    // because the screen is now empty.
    task_list[current_task_idx].first_x = 10000;
    task_list[current_task_idx].first_y = 10000;
    task_list[current_task_idx].last_x = 0;
    task_list[current_task_idx].last_y = 0;
    task_list[current_task_idx].has_drawn = 0;
}
else if (regs->eax == 6) { // Syscall 6: DRAW_RECT
    int x = regs->ebx;
    int y = regs->ecx;
    int w = regs->edx;
    int h = regs->esi;          // We'll use ESI for height
    uint32_t color = regs->edi; // We'll use EDI for color
    int id = current_task_idx;

    if (task_list[id].has_window) {
        // --- WINDOW MANAGER MODE ---
        // Draw into the private canvas in RAM, NOT the screen!
        for (int iy = 0; iy < h; iy++) {
            for (int ix = 0; ix < w; ix++) {
                int draw_x = x + ix;
                int draw_y = y + iy;
                
                // Safety: Prevent memory corruption by clipping pixels 
                // that try to draw outside the window boundaries!
                if (draw_x >= 0 && draw_x < task_list[id].win_w && 
                    draw_y >= 0 && draw_y < task_list[id].win_h) {
                    
                    // Math to find a 2D pixel in a 1D array: (Y * Width) + X
                    int pixel_index = (draw_y * task_list[id].win_w) + draw_x;
                    task_list[id].window_buffer[pixel_index] = color;
                }
            }
        }
        task_list[id].has_drawn = 1; // Flag that this window needs a repaint
        
    } else {
        // --- LEGACY CLI MODE ---
        // Keep your old behavior for apps that don't know about the GUI yet
        if (x < task_list[id].first_x) task_list[id].first_x = x;
        if (y < task_list[id].first_y) task_list[id].first_y = y;
        if (x + w > task_list[id].last_x) task_list[id].last_x = x + w;
        if (y + h > task_list[id].last_y) task_list[id].last_y = y + h;

        task_list[id].has_drawn = 1;
        VESA_draw_rect(x, y, w, h, color);
    }
}
else if (regs->eax == 7) { // Syscall 7: PRINT_STR
    // EBX contains the relative offset. Add code_ptr to get the actual address.
    char* str = (char*)(regs->ebx + task_list[current_task_idx].code_ptr);
    int x = regs->ecx;
    int y = regs->edx;
    uint32_t color = regs->edi;
    int id = current_task_idx;

    int i = 0;
    while (str[i] != '\0') {
        VESA_draw_char(str[i], x + (i * 8), y, color);
        i++;
    }

    // Update Bounding Box for auto-cleanup (String width is i * 8)
    if (x < task_list[id].first_x) task_list[id].first_x = x;
    if (y < task_list[id].first_y) task_list[id].first_y = y;
    if (x + (i * 8) > task_list[id].last_x) task_list[id].last_x = x + (i * 8);
    if (y + 8 > task_list[id].last_y) task_list[id].last_y = y + 8;

    task_list[id].has_drawn = 1;
}
else if (regs->eax == 8) { // Syscall 8: CREATE_WINDOW
        int x = regs->ebx; // Requested X
        int y = regs->ecx; // Requested Y
        int w = regs->edx; // Requested Width
        int h = regs->esi; // Requested Height
        int id = current_task_idx;

        task_list[id].window_buffer = (uint32_t*)kmalloc(w * h * 4);
        
        if (task_list[id].window_buffer) {
            task_list[id].win_w = w;
            task_list[id].win_h = h;
            
            // Set the window exactly where the script asked for it!
            task_list[id].win_x = x; 
            task_list[id].win_y = y; 
            
            task_list[id].has_window = 1;

            // Fill the background of the window with dark gray
            for(int i = 0; i < (w * h); i++) {
                task_list[id].window_buffer[i] = 0x222222; 
            }
        }
    }

  else if (regs->eax == 9) { // PRINT_NUMBER
    uint32_t val = regs->ebx;
    int x = regs->ecx;
    int y = regs->edx;
    uint32_t color = regs->edi;

    char buf[12];
    itoa(val, buf, 10); // Your kernel's integer-to-ascii function
    VESA_print_at(buf, x, y, color);
}

}

