#include "idt.h"
#include "io.h"
#include "lib.h"
#include "task.h"
#include "vesa.h"
#include "kheap.h"
uint32_t timer_frequency = 0; // Global variable to store the frequency
extern struct task task_list[];
extern int current_task_idx;
extern void isr0(); // Declaration of the assembly label
volatile uint32_t system_ticks = 0;
struct idt_entry idt[256];
struct idt_ptr idtp;
extern void isr128_stub();

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
    // ... add more if you like
};


// Ensure your 'struct registers' is defined in idt.h exactly 
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
    // ADD THIS: Handle the Timer (IRQ 0 -> INT 32)
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

// Define this at the top of idt.c
uint32_t next_stack_ptr = 0;
extern uint32_t target_fps;
void timer_handler(struct registers *regs) {
    system_ticks++;
    // Calculate how many ticks must pass for one frame
    // Example: 1000Hz / 60 FPS = 16 ticks
    uint32_t ticks_per_frame = timer_frequency / target_fps;

    // Safety: Ensure we don't divide by zero or get a 0 interval
    if (ticks_per_frame == 0) ticks_per_frame = 1;

    // Now it updates based on your variable!
    if (system_ticks % ticks_per_frame == 0) {
        VESA_flip();
    }if (multitasking_enabled){
    // --- NEW: CPU Accounting ---
    // Increment the tick count for the task that was just interrupted.
    // This tracks how much actual CPU time each process is getting.
    if (task_list[current_task_idx].state != 0) {
        task_list[current_task_idx].total_ticks++;
    }

    // 1. Update sleeping tasks
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_list[i].state == 2) { // 2 = SLEEPING
            if (task_list[i].sleep_ticks > 0) {
                task_list[i].sleep_ticks--;
            } 
            // Check again after decrementing to wake up immediately if time is up
            if (task_list[i].sleep_ticks == 0) {
                task_list[i].state = 1; // Wake up! Set to READY
            }
        }
    }

    // 2. Save current task ESP
    // This stores the stack pointer so we can resume this task later
    task_list[current_task_idx].esp = (uint32_t)regs;

    // 3. Find NEXT task that is READY (state 1)
    int next_task = (current_task_idx + 1) % MAX_TASKS;
    int check_count = 0;
    while (task_list[next_task].state != 1 && check_count < MAX_TASKS) {
        next_task = (next_task + 1) % MAX_TASKS;
        check_count++;
    }

    // Update global pointers for the Assembly stub to perform the switch
    current_task_idx = next_task;
    next_stack_ptr = task_list[current_task_idx].esp;
  }
    // Send End of Interrupt (EOI) to the PIC
    outb(0x20, 0x20);
}
// Track shift state globally in idt.c or io.c

void keyboard_handler(struct registers *regs) {
    (void)regs; // Silence unused warning
    uint8_t scancode = inb(0x60);

    // 1. Update states IMMEDIATELY
    if (scancode == 0x1D) {
        ctrl_state = 1;
    } else if (scancode == 0x9D) {
        ctrl_state = 0;
    } else if (scancode == 0x2A || scancode == 0x36) {
        shift_state = 1;
    } else if (scancode == 0xAA || scancode == 0xB6) {
        shift_state = 0;
    } 
    // 2. Process Actual Key Presses
    else if (!(scancode & 0x80)) {
        char c = scancode_to_ascii(scancode, shift_state);
        
        if (c != 0) {
            if (ctrl_state) {
                if (c == 's' || c == 'S') {
                    c = 19; // DC3
                    //VESA_print_at("CTRL+S DETECTED", 100, 100, 0x00FF00);
                } else if (c == 'q' || c == 'Q') {
                    c = 17; // DC1
                    //VESA_print_at("CTRL+Q DETECTED", 100, 120, 0xFF0000);
                }else if (c == 'p' || c == 'P') {
                    c = 16; 
                    //VESA_print_at("CTRL+P DETECTED", 100, 120, 0xFF0000);
                }
            }
            keyboard_push_char(c); 
        }
    }

    outb(0x20, 0x20);
}

void syscall_handler(struct registers *regs) {
    if (regs->eax == 1) { // DRAW_CHAR
    char c = (char)regs->ebx;
    int x = regs->ecx;
    int y = regs->edx;
    int id = current_task_idx;

    // Expand bounds to include this new character
    if (x < task_list[id].first_x) task_list[id].first_x = x;
    if (y < task_list[id].first_y) task_list[id].first_y = y;
    
    // Check the right and bottom edges (8 pixels for a standard font)
    if (x + 8 > task_list[id].last_x) task_list[id].last_x = x + 8;
    if (y + 8 > task_list[id].last_y) task_list[id].last_y = y + 8;

    task_list[id].has_drawn = 1;
    VESA_draw_char(c, x, y, 0xFFFFFF); 
}
    else if (regs->eax == 2) { // Get Ticks
        regs->eax = system_ticks; 
    }

else if (regs->eax == 3) { // Syscall 3: Sleep(ms)
    uint32_t ms = regs->ebx;
    
    // 1. Convert Milliseconds to Ticks based on current frequency
    // Formula: Ticks = (ms * frequency) / 1000
    uint32_t ticks_to_sleep = (ms * timer_frequency) / 1000;

    // Safety: Ensure we sleep at least 1 tick if ms > 0
    if (ms > 0 && ticks_to_sleep == 0) {
        ticks_to_sleep = 1;
    }

    task_list[current_task_idx].sleep_ticks = ticks_to_sleep; 
    task_list[current_task_idx].state = 2; // Set state to SLEEPING
    task_list[current_task_idx].esp = (uint32_t)regs;

    // 2. Find the next READY task
    int next_task = (current_task_idx + 1) % MAX_TASKS;
    int found = -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        int idx = (next_task + i) % MAX_TASKS;
        if (task_list[idx].state == 1) { // Found a READY task
            found = idx;
            break;
        }
    }

    // 3. SAFETY: If NO other task is ready (e.g., shell is alone and no idle task),
    // we cannot put the current task to sleep or the CPU will have nothing to run.
    if (found == -1 || found == current_task_idx) {
        task_list[current_task_idx].state = 1; // Keep it READY
        next_stack_ptr = 0;                    // Don't switch stacks
        return; 
    }

    // 4. Perform the switch
    current_task_idx = found;
    next_stack_ptr = task_list[found].esp;
  }
  else if (regs->eax == 4) { // Syscall 4: Exit/Terminate
    kprintf_unsync("Task %d exited.\n", current_task_idx);
    task_list[current_task_idx].state = 0; // Set to DEAD
    if (task_list[current_task_idx].has_drawn) {
        int w = task_list[current_task_idx].last_x - task_list[current_task_idx].first_x;
        int h = task_list[current_task_idx].last_y - task_list[current_task_idx].first_y;
        
        // Safety check to prevent massive unsigned underflow clears
        if (w > 0 && w < 2000 && h > 0 && h < 2000) {
            VESA_clear_region(task_list[current_task_idx].first_x, task_list[current_task_idx].first_y, w, h);
            VESA_flip(); 
        }
    }
    // --- CLEANUP STACK ---
    if (task_list[current_task_idx].stack_ptr != NULL) {
        kfree(task_list[current_task_idx].stack_ptr);
        task_list[current_task_idx].stack_ptr = NULL;
    }

    // --- CLEANUP CODE (The missing 4KB!) ---
    if (task_list[current_task_idx].code_ptr != NULL) {
        kfree(task_list[current_task_idx].code_ptr);
        task_list[current_task_idx].code_ptr = NULL; 
    }
    // Immediately switch to another task
    int next_task = (current_task_idx + 1) % MAX_TASKS;
    while(task_list[next_task].state != 1) next_task = (next_task + 1) % MAX_TASKS;
    
    current_task_idx = next_task;
    next_stack_ptr = task_list[next_task].esp;
  }
else if (regs->eax == 5) { // Syscall 5: Clear Screen
    VESA_clear();
    // It's good practice to also reset the kernel's bounding box 
    // because the screen is now empty.
    task_list[current_task_idx].first_x = 0;
    task_list[current_task_idx].first_y = 0;
    task_list[current_task_idx].last_x = 0;
    task_list[current_task_idx].last_y = 0;
    task_list[current_task_idx].has_drawn = 0;
}
else if (regs->eax == 6) { // DRAW_RECT
    int x = regs->ebx;
    int y = regs->ecx;
    int w = regs->edx;
    int h = regs->esi;          // We'll use ESI for height
    uint32_t color = regs->edi; // We'll use EDI for color

    // Update the Task's bounding box for auto-cleanup
    if (x < task_list[current_task_idx].first_x) task_list[current_task_idx].first_x = x;
    if (y < task_list[current_task_idx].first_y) task_list[current_task_idx].first_y = y;
    if (x + w > task_list[current_task_idx].last_x) task_list[current_task_idx].last_x = x + w;
    if (y + h > task_list[current_task_idx].last_y) task_list[current_task_idx].last_y = y + h;

    task_list[current_task_idx].has_drawn = 1;
    
    VESA_draw_rect(x, y, w, h, color);
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
}

// Helper to emit a MOV instruction for a specific register
void emit_mov(uint8_t reg_code, uint32_t val, uint8_t* out_buf, uint32_t* pos) {
    out_buf[(*pos)++] = reg_code;
    kmemcpy(&out_buf[*pos], &val, 4);
    *pos += 4;
}

void assemble_line(const char* line, uint8_t* out_buf, uint32_t* pos) {
    char cmd[32];
    char arg_str[32];
    
    // Get the command name
    const char* ptr = get_token(line, cmd);
    if (!ptr) return;

    if (kstrcmp(cmd, "DRAW_CHAR") == 0) {
        // Syntax: DRAW_CHAR <ascii> <x> <y>
        uint32_t char_val, x, y;
        
        ptr = get_token(ptr, arg_str);
        char_val = katoi(arg_str);
        
        ptr = get_token(ptr, arg_str);
        x = katoi(arg_str);
        
        ptr = get_token(ptr, arg_str);
        y = katoi(arg_str);

        emit_mov(0xB8, 1, out_buf, pos);    // MOV EAX, 1
        emit_mov(0xBB, char_val, out_buf, pos); // MOV EBX, char
        emit_mov(0xB9, x, out_buf, pos);    // MOV ECX, x
        emit_mov(0xBA, y, out_buf, pos);    // MOV EDX, y
        
        out_buf[(*pos)++] = 0xCD; // INT 0x80
        out_buf[(*pos)++] = 0x80;
    }
    else if (kstrcmp(cmd, "GET_TICKS") == 0) {
        emit_mov(0xB8, 2, out_buf, pos); // MOV EAX, 2
        out_buf[(*pos)++] = 0xCD; 
        out_buf[(*pos)++] = 0x80;
    }
    else if (kstrcmp(cmd, "SLEEP") == 0) {
        uint32_t ms;
        ptr = get_token(ptr, arg_str);
        ms = katoi(arg_str);

        emit_mov(0xB8, 3, out_buf, pos); // EAX=3
        emit_mov(0xBB, ms, out_buf, pos); // EBX=ms
        out_buf[(*pos)++] = 0xCD; 
        out_buf[(*pos)++] = 0x80;
    }
    else if (kstrcmp(cmd, "EXIT") == 0) {
        emit_mov(0xB8, 4, out_buf, pos); // EAX=4
        out_buf[(*pos)++] = 0xCD; 
        out_buf[(*pos)++] = 0x80;
    }
    else if (kstrcmp(cmd, "NOP") == 0) {
        out_buf[(*pos)++] = 0x90;
    }
    else if (kstrcmp(cmd, "HLT") == 0) {
        out_buf[(*pos)++] = 0xF4;
    }
else if (kstrcmp(cmd, "RECT") == 0) {
    // Syntax: RECT x y w h color
    char arg_str[32];
    uint32_t x, y, w, h, color;

    ptr = get_token(ptr, arg_str); x = katoi(arg_str);
    ptr = get_token(ptr, arg_str); y = katoi(arg_str);
    ptr = get_token(ptr, arg_str); w = katoi(arg_str);
    ptr = get_token(ptr, arg_str); h = katoi(arg_str);
    ptr = get_token(ptr, arg_str); color = katoi(arg_str);

    emit_mov(0xB8, 6, out_buf, pos);     // EAX = 6
    emit_mov(0xBB, x, out_buf, pos);     // EBX = x
    emit_mov(0xB9, y, out_buf, pos);     // ECX = y
    emit_mov(0xBA, w, out_buf, pos);     // EDX = w
    emit_mov(0xBE, h, out_buf, pos);     // ESI = h (Opcode 0xBE)
    emit_mov(0xBF, color, out_buf, pos); // EDI = color (Opcode 0xBF)
    
    out_buf[(*pos)++] = 0xCD; // INT 0x80
    out_buf[(*pos)++] = 0x80;
}
else if (kstrcmp(cmd, "CLEAR") == 0) {
    // We only need to set EAX to 5 and trigger the interrupt
    emit_mov(0xB8, 5, out_buf, pos); // MOV EAX, 5
    out_buf[(*pos)++] = 0xCD;        // INT 0x80
    out_buf[(*pos)++] = 0x80;
}
else if (kstrcmp(cmd, "PRINT") == 0) {
    char str_val[128];
    char tmp[32];
    uint32_t x, y, color;

    // 1. Extract the string between quotes
    const char* p = line;
    while (*p != '\"' && *p != '\0') p++;
    int s_idx = 0;
    if (*p == '\"') {
        p++;
        while (*p != '\"' && *p != '\0' && s_idx < 127) str_val[s_idx++] = *p++;
        str_val[s_idx++] = '\0'; // Include null terminator
        if (*p == '\"') p++;
    }

    // 2. Extract X, Y, and Color
    p = get_token(p, tmp); x = katoi(tmp);
    p = get_token(p, tmp); y = katoi(tmp);
    p = get_token(p, tmp); 
    color = (tmp[0] == '0' && tmp[1] == 'x') ? katoh(tmp) : katoi(tmp);

    // 3. Emit JMP over the string data (2 bytes: EB + length)
    out_buf[(*pos)++] = 0xEB; 
    out_buf[(*pos)++] = (uint8_t)s_idx;

    // 4. Write the String Data into the binary
    uint32_t str_offset = *pos;
    for (int j = 0; j < s_idx; j++) {
        out_buf[(*pos)++] = (uint8_t)str_val[j];
    }

    // 5. Emit Syscall Setup
    emit_mov(0xB8, 7, out_buf, pos);          // EAX = 7
    emit_mov(0xBB, str_offset, out_buf, pos); // EBX = Offset to string
    emit_mov(0xB9, x, out_buf, pos);          // ECX = X
    emit_mov(0xBA, y, out_buf, pos);          // EDX = Y
    emit_mov(0xBF, color, out_buf, pos);      // EDI = Color
    
    out_buf[(*pos)++] = 0xCD; // INT 0x80
    out_buf[(*pos)++] = 0x80;
}
}
