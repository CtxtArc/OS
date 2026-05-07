#include "idt.h"
#include "io.h"
#include "lib.h"
#include "task.h"
#include "vesa.h"
#include "kheap.h"
#include "assembler.h"

uint32_t timer_frequency = 0;
extern volatile struct task task_list[MAX_TASKS];
extern int current_task_idx;
extern void isr0();
volatile uint32_t system_ticks = 0;
struct idt_entry idt[256];
struct idt_ptr idtp;
extern void isr128_stub();
extern int keyboard_focus_tid;
extern int vesa_updating;
extern void idle_task_code();

char *exception_messages[] = {
    "Division By Zero", "Debug", "Non Maskable Interrupt", "Breakpoint",
    "Into Detected Overflow", "Out of Bounds", "Invalid Opcode", "No Coprocessor",
    "Double Fault", "Coprocessor Segment Overrun", "Bad TSS", "Segment Not Present",
    "Stack Fault", "General Protection Fault", "Page Fault", "Unknown Interrupt"
};

void isr_handler(struct registers *r) {
    VESA_clear();
    for(int i = 0; i < 80 * 25; i++) {
        ((uint16_t*)0xB8000)[i] = (uint16_t)' ' | (uint16_t)0x1F << 8;
    }
    task_list[0].cursor_x = 0;
    task_list[0].cursor_y = 0;

    kprintf_color(COLOR_WHITE, "--- KERNEL PANIC ---\n");
    kprintf_color(COLOR_WHITE, "Interrupt: %d (%s)\n", r->int_no, exception_messages[r->int_no]);
    kprintf_color(COLOR_WHITE, "EIP: %x  EAX: %x  EBX: %x\n", r->eip, r->eax, r->ebx);
    kprintf_color(COLOR_WHITE, "ECX: %x  EDX: %x\n", r->ecx, r->edx);

    while(1) __asm__("hlt");
}

void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low  = (base & 0xFFFF);
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].sel       = sel;
    idt[num].always0   = 0;
    idt[num].flags     = flags;
}

int ctrl_state  = 0;
int shift_state = 0;
int alt_state   = 0;

void pic_remap() {
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, 0xFC); outb(0xA1, 0xFF);
}

void idt_init() {
    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base  = (uint32_t)&idt;

    for(int i = 0; i < 256; i++) idt_set_gate(i, 0, 0, 0);
    idt_set_gate(128, (uint32_t)isr128_stub, 0x08, 0x8E);

    extern void irq0_handler();
    idt_set_gate(32, (uint32_t)irq0_handler, 0x08, 0x8E);
    idt_set_gate(0,  (uint32_t)isr0,         0x08, 0x8E);

    extern void isr13();
    idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8E);

    extern void irq1_handler();
    idt_set_gate(33, (uint32_t)irq1_handler, 0x08, 0x8E);

    __asm__ volatile("lidt (%0)" : : "r" (&idtp));
}

void timer_init(uint32_t frequency) {
    if (frequency == 0) frequency = 1;
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

    uint32_t ticks_per_frame = timer_frequency / target_fps;
    if (ticks_per_frame == 0) ticks_per_frame = 1;
    if (system_ticks % ticks_per_frame == 0) {
        if (!vesa_updating) VESA_flip();
    }

    if (multitasking_enabled) {
        if (task_list[current_task_idx].state != 0)
            task_list[current_task_idx].total_ticks++;

        for (int i = 0; i < MAX_TASKS; i++) {
            if (task_list[i].state == 2) {
                if (task_list[i].sleep_ticks > 0) task_list[i].sleep_ticks--;
                if (task_list[i].sleep_ticks == 0) task_list[i].state = 1;
            }
        }

        task_list[current_task_idx].esp = (uint32_t)regs;

        int next_task = (current_task_idx + 1) % MAX_TASKS;
        int found = -1;
        for (int i = 0; i < MAX_TASKS; i++) {
            int idx = (next_task + i) % MAX_TASKS;
            if (task_list[idx].state == 1) { found = idx; break; }
        }

        if (found == -1) {
            if (task_list[current_task_idx].state == 1) found = current_task_idx;
            else found = 0;
        }

        current_task_idx = found;
        next_stack_ptr   = task_list[current_task_idx].esp;
    }
    outb(0x20, 0x20);
}

void keyboard_handler(struct registers *regs) {
    (void)regs;
    uint8_t scancode = inb(0x60);

    if      (scancode == 0x1D)                          { ctrl_state  = 1; }
    else if (scancode == 0x9D)                          { ctrl_state  = 0; }
    else if (scancode == 0x2A || scancode == 0x36)      { shift_state = 1; }
    else if (scancode == 0xAA || scancode == 0xB6)      { shift_state = 0; }
    else if (scancode == 0x3B)                          { keyboard_focus_tid = 0; }
    else if (!(scancode & 0x80)) {
        char c = scancode_to_ascii(scancode, shift_state);
        if (c != 0) {
            
            // --- THE FIX: ASCII Control Code Math ---
            if (ctrl_state) {
                if (c >= 'a' && c <= 'z') {
                    c = c - 'a' + 1; // Turns 's' into 19, 'q' into 17, etc.
                } else if (c >= 'A' && c <= 'Z') {
                    c = c - 'A' + 1; 
                }
            }
            // ----------------------------------------

            volatile struct task *target = &task_list[keyboard_focus_tid];

            // WARNING FIX: Removed < 0 check. 
            if (target->kbd_head >= 64) target->kbd_head = 0;
            if (target->kbd_tail >= 64) target->kbd_tail = 0;

            int next = (target->kbd_head + 1) % 64;
            if (next != target->kbd_tail) {
                target->kbd_buffer[target->kbd_head] = c;
                target->kbd_head = next;
                if (target->state == 3) target->state = 1; 
            }
        }
    }
    outb(0x20, 0x20);
}
// RESTORED ASSEMBLER DEPS
void emit_mov(uint8_t reg_code, uint32_t val, uint8_t* out_buf, uint32_t* pos) {
    out_buf[(*pos)++] = reg_code;
    kmemcpy(&out_buf[*pos], &val, 4);
    *pos += 4;
}

void emit_load(uint8_t reg_opcode, const char* arg, uint8_t* out_buf, uint32_t* pos) {
    if ((arg[0] >= '0' && arg[0] <= '9') || arg[0] == '-') {
        uint32_t val = (arg[0] == '0' && arg[1] == 'x') ? katoh(arg) : katoi(arg);
        out_buf[(*pos)++] = reg_opcode;
        kmemcpy(&out_buf[*pos], &val, 4);
        *pos += 4;
    } else {
        uint32_t offset = get_var_offset(arg);
        out_buf[(*pos)++] = 0x8B;
        if      (reg_opcode == 0xB8) out_buf[(*pos)++] = 0x05;
        else if (reg_opcode == 0xBB) out_buf[(*pos)++] = 0x1D;
        else if (reg_opcode == 0xB9) out_buf[(*pos)++] = 0x0D;
        else if (reg_opcode == 0xBA) out_buf[(*pos)++] = 0x15;
        else if (reg_opcode == 0xBE) out_buf[(*pos)++] = 0x35;
        else if (reg_opcode == 0xBF) out_buf[(*pos)++] = 0x3D;
        kmemcpy(&out_buf[*pos], &offset, 4);
        *pos += 4;
    }
}

void syscall_handler(struct registers *regs) {
    __asm__ volatile("sti");

    struct multiboot_info* mbi = VESA_get_boot_info();
    uint32_t sw = mbi->framebuffer_width;
    uint32_t sh = mbi->framebuffer_height;
    int id = current_task_idx;

    if (regs->eax == 1) { // DRAW_CHAR
        char c    = (char)regs->ebx;
        int x     = regs->ecx;
        int y     = regs->edx;

        if (task_list[id].has_window && task_list[id].window_buffer) {
            extern uint8_t font8x8_basic[128][8];
            uint8_t* glyph = font8x8_basic[(int)c];
            
            for (int row = 0; row < 8; row++) {
                for (int col = 0; col < 8; col++) {
                    int draw_x = x + col;
                    int draw_y = y + row;
                    if (draw_x >= 0 && draw_x < task_list[id].win_w &&
                        draw_y >= 0 && draw_y < task_list[id].win_h) {
                        task_list[id].window_buffer[(draw_y * sw) + draw_x] = 
                            (glyph[row] & (1 << (7 - col))) ? 0xFFFFFF : 0x222222;
                    }
                }
            }
            task_list[id].has_drawn = 1;
        } else {
            VESA_draw_char(c, x, y, 0xFFFFFF);
        }
    }
    else if (regs->eax == 2) { // GET_TICKS
        regs->eax = system_ticks;
    }
    else if (regs->eax == 3) { // SLEEP(ms)
        uint32_t ms = regs->ebx;
        uint32_t ticks_to_sleep = (ms * timer_frequency) / 1000;
        if (ms > 0 && ticks_to_sleep == 0) ticks_to_sleep = 1;
        task_list[id].sleep_ticks = ticks_to_sleep;
        task_list[id].state = 2; // SLEEPING
    }
    else if (regs->eax == 4) { // EXIT
        task_list[id].state = 0; // DEAD
        if (task_list[id].window_buffer) {
            kfree(task_list[id].window_buffer);
            task_list[id].window_buffer = NULL;
        }
        refresh_tiling_layout(); // Re-tile remaining windows
        regs->eip = (uint32_t)idle_task_code;
    }
    else if (regs->eax == 6) { // DRAW_RECT
        int rx = regs->ebx; int ry = regs->ecx;
        int rw = regs->edx; int rh = regs->esi;
        uint32_t color = regs->edi;

        if (task_list[id].has_window && task_list[id].window_buffer) {
            for (int iy = 0; iy < rh; iy++) {
                for (int ix = 0; ix < rw; ix++) {
                    int dx = rx + ix; int dy = ry + iy;
                    if (dx >= 0 && dx < task_list[id].win_w && dy >= 0 && dy < task_list[id].win_h) {
                        task_list[id].window_buffer[(dy * sw) + dx] = color;
                    }
                }
            }
            task_list[id].has_drawn = 1;
        }
    }
    else if (regs->eax == 7) { // PRINT_STR
        uint32_t aligned_code = ((uint32_t)task_list[id].code_ptr + 0xFFF) & 0xFFFFF000;
        char* str = (char*)(regs->ebx + aligned_code);
        
        int sx = regs->ecx; 
        int sy = regs->edx;
        uint32_t color = regs->edi;

        for (int i = 0; str[i] != '\0'; i++) {
            int cur_x = sx + (i * 8);
            unsigned char c = (unsigned char)str[i];
            if (c > 127) c = '?'; 

            extern uint8_t font8x8_basic[128][8];
            uint8_t* glyph = font8x8_basic[c];
            
            for (int row = 0; row < 8; row++) {
                for (int col = 0; col < 8; col++) {
                    if (glyph[row] & (1 << (7 - col))) {
                        int dx = cur_x + col; 
                        int dy = sy + row;
                        if (dx >= 0 && dx < task_list[id].win_w && dy >= 0 && dy < task_list[id].win_h) {
                            task_list[id].window_buffer[(dy * sw) + dx] = color;
                        }
                    }
                }
            }
        }
        task_list[id].has_drawn = 1;
    }
    else if (regs->eax == 8) { // CREATE_WINDOW
        task_list[id].has_window = 1;
        if (task_list[id].window_buffer == NULL) {
            task_list[id].window_buffer = (uint32_t*)kmalloc(sw * sh * 4);
            if (!task_list[id].window_buffer) {
                kprintf_unsync("OOM: Window failed\n");
                task_list[id].has_window = 0;
                return;
            }
            for (uint32_t i = 0; i < (sw * sh); i++)
                task_list[id].window_buffer[i] = 0x222222;
        }
        task_list[id].cursor_x = 0;
        task_list[id].cursor_y = 0;
        refresh_tiling_layout();
        task_list[id].has_drawn = 1;
    }
}
