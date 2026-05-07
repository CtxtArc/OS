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

extern void irq1_handler();

extern char buffer[128];
extern int buffer_idx;
extern int shift_state;
extern int ctrl_state;

char kbd_buffer[KEYBOARD_BUFFER_SIZE];
int kbd_head = 0;
int kbd_tail = 0;

int ctrl_state  = 0;
int shift_state = 0;
int alt_state   = 0;

void pic_remap() {
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0xFC);
    outb(0xA1, 0xFF);
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
            volatile struct task *target = &task_list[keyboard_focus_tid];
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

    // Cache the screen stride once — used by every drawing syscall.
    // window_buffer is always allocated at full framebuffer_width * height,
    // so this is the correct row stride regardless of the tile's win_w.
    uint32_t sw = VESA_get_boot_info()->framebuffer_width;

    if (regs->eax == 1) { // DRAW_CHAR
        char c   = (char)regs->ebx;
        int x    = regs->ecx;
        int y    = regs->edx;
        int id   = current_task_idx;

        if (task_list[id].has_window) {
            extern uint8_t font8x8_basic[128][8];
            uint8_t* glyph      = font8x8_basic[(int)c];
            uint32_t text_color = 0xFFFFFF;
            uint32_t bg_color   = 0x222222;

            for (int row = 0; row < 8; row++) {
                uint8_t data = glyph[row];
                for (int col = 0; col < 8; col++) {
                    int draw_x = x + col;
                    int draw_y = y + row;

                    // Clip to visible tile bounds
                    if (draw_x >= 0 && draw_x < task_list[id].win_w &&
                        draw_y >= 0 && draw_y < task_list[id].win_h) {
                        // FIX: use sw (framebuffer_width) as stride, not win_w
                        int pixel_index = (draw_y * sw) + draw_x;
                        task_list[id].window_buffer[pixel_index] =
                            (data & (1 << (7 - col))) ? text_color : bg_color;
                    }
                }
            }
            task_list[id].has_drawn = 1;

        } else {
            if (x < task_list[id].first_x) task_list[id].first_x = x;
            if (y < task_list[id].first_y) task_list[id].first_y = y;
            if (x + 8 > task_list[id].last_x) task_list[id].last_x = x + 8;
            if (y + 8 > task_list[id].last_y) task_list[id].last_y = y + 8;
            task_list[id].has_drawn = 1;
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
        task_list[current_task_idx].sleep_ticks = ticks_to_sleep;
        task_list[current_task_idx].state = 2;
    }
    else if (regs->eax == 4) { // EXIT
        kprintf_unsync("Task %d exited.\n", current_task_idx);
        task_list[current_task_idx].state = 0;

        if (task_list[current_task_idx].has_drawn) {
            int w = task_list[current_task_idx].last_x - task_list[current_task_idx].first_x;
            int h = task_list[current_task_idx].last_y - task_list[current_task_idx].first_y;
            if (w > 0 && w < 2000 && h > 0 && h < 2000) {
                VESA_clear_region(task_list[current_task_idx].first_x,
                                  task_list[current_task_idx].first_y, w, h);
                VESA_flip();
            }
        }

        if (task_list[current_task_idx].stack_ptr) {
            kfree(task_list[current_task_idx].stack_ptr);
            task_list[current_task_idx].stack_ptr = NULL;
        }
        if (task_list[current_task_idx].code_ptr) {
            kfree(task_list[current_task_idx].code_ptr);
            task_list[current_task_idx].code_ptr = NULL;
        }
        if (task_list[current_task_idx].window_buffer) {
            kfree(task_list[current_task_idx].window_buffer);
            task_list[current_task_idx].window_buffer = NULL;
        }

        regs->eip = (uint32_t)idle_task_code;
    }
    else if (regs->eax == 5) { // CLEAR_SCREEN
        VESA_clear();
        task_list[current_task_idx].first_x  = 10000;
        task_list[current_task_idx].first_y  = 10000;
        task_list[current_task_idx].last_x   = 0;
        task_list[current_task_idx].last_y   = 0;
        task_list[current_task_idx].has_drawn = 0;
    }
    else if (regs->eax == 6) { // DRAW_RECT
        int x        = regs->ebx;
        int y        = regs->ecx;
        int w        = regs->edx;
        int h        = regs->esi;
        uint32_t color = regs->edi;
        int id       = current_task_idx;

        if (task_list[id].has_window) {
            for (int iy = 0; iy < h; iy++) {
                for (int ix = 0; ix < w; ix++) {
                    int draw_x = x + ix;
                    int draw_y = y + iy;

                    // Clip to visible tile bounds
                    if (draw_x >= 0 && draw_x < task_list[id].win_w &&
                        draw_y >= 0 && draw_y < task_list[id].win_h) {
                        // FIX: use sw (framebuffer_width) as stride, not win_w
                        int pixel_index = (draw_y * sw) + draw_x;
                        task_list[id].window_buffer[pixel_index] = color;
                    }
                }
            }
            task_list[id].has_drawn = 1;

        } else {
            if (x     < task_list[id].first_x) task_list[id].first_x = x;
            if (y     < task_list[id].first_y) task_list[id].first_y = y;
            if (x + w > task_list[id].last_x)  task_list[id].last_x  = x + w;
            if (y + h > task_list[id].last_y)  task_list[id].last_y  = y + h;
            task_list[id].has_drawn = 1;
            VESA_draw_rect(x, y, w, h, color);
        }
    }
    else if (regs->eax == 7) { // PRINT_STR
        char* str      = (char*)(regs->ebx + task_list[current_task_idx].code_ptr);
        int x          = regs->ecx;
        int y          = regs->edx;
        uint32_t color = regs->edi;
        int id         = current_task_idx;

        int i = 0;
        while (str[i] != '\0') {
            VESA_draw_char(str[i], x + (i * 8), y, color);
            i++;
        }

        if (x         < task_list[id].first_x) task_list[id].first_x = x;
        if (y         < task_list[id].first_y) task_list[id].first_y = y;
        if (x+(i*8)   > task_list[id].last_x)  task_list[id].last_x  = x + (i * 8);
        if (y + 8     > task_list[id].last_y)  task_list[id].last_y  = y + 8;
        task_list[id].has_drawn = 1;
    }
    else if (regs->eax == 8) { // CREATE_WINDOW
        int id = current_task_idx;
        uint32_t sh = VESA_get_boot_info()->framebuffer_height;

        task_list[id].has_window = 1;

        if (task_list[id].window_buffer == NULL) {
            task_list[id].window_buffer = (uint32_t*)kmalloc(sw * sh * 4);
            for (uint32_t i = 0; i < (sw * sh); i++)
                task_list[id].window_buffer[i] = 0x222222;
        }

        task_list[id].cursor_x = 0;
        task_list[id].cursor_y = 0;
        refresh_tiling_layout();
        task_list[id].has_drawn = 1;
    }
    else if (regs->eax == 9) { // PRINT_NUMBER
        uint32_t val   = regs->ebx;
        int x          = regs->ecx;
        int y          = regs->edx;
        uint32_t color = regs->edi;

        char buf[12];
        itoa(val, buf, 10);
        VESA_print_at(buf, x, y, color);
    }
}
