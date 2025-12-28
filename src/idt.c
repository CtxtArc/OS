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

