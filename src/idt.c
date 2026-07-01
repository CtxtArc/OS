#include "idt.h"
#include "assembler.h"
#include "io.h"
#include "kheap.h"
#include "lib.h"
#include "task.h"
#include "vesa.h"
#include "vfs.h"
#include "wm.h"
#include <stdint.h>

uint32_t timer_frequency = 0;
extern volatile struct task task_list[MAX_TASKS];
extern volatile int current_task_idx;
extern void isr0();
volatile uint32_t system_ticks = 0;
struct idt_entry idt[256];
struct idt_ptr idtp;
extern void isr128_stub();
extern uint32_t schedule_next(uint32_t current_esp);
// THE FIX: Mark this volatile so the Compositor loop actually sees the changes!
extern volatile int keyboard_focus_tid;

extern int vesa_updating;
extern void idle_task_code();
extern volatile int multitasking_enabled;

char *exception_messages[] = {"Division By Zero",
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
                              "Unknown Interrupt"};

void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
  idt[num].base_low = (base & 0xFFFF);
  idt[num].base_high = (base >> 16) & 0xFFFF;
  idt[num].sel = sel;
  idt[num].always0 = 0;
  idt[num].flags = flags;
}

int ctrl_state = 0;
int shift_state = 0;
int alt_state = 0;
int altgr_state = 0;

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
  idtp.base = (uint32_t)&idt;

  for (int i = 0; i < 256; i++)
    idt_set_gate(i, 0, 0, 0);

  // System calls
  idt_set_gate(128, (uint32_t)isr128_stub, 0x08, 0x8E);

  // Hardware IRQs
  extern void irq0_handler();
  extern void irq1_handler();
  idt_set_gate(32, (uint32_t)irq0_handler, 0x08, 0x8E);
  idt_set_gate(33, (uint32_t)irq1_handler, 0x08, 0x8E);

  // CPU Exceptions
  extern void isr0();
  extern void isr13();
  extern void isr14();

  idt_set_gate(0, (uint32_t)isr0, 0x08, 0x8E);
  idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8E);
  idt_set_gate(14, (uint32_t)isr14, 0x08, 0x8E);

  __asm__ volatile("lidt (%0)" : : "r"(&idtp));
}

void timer_init(uint32_t frequency) {
  if (frequency == 0)
    frequency = 1;
  timer_frequency = frequency;

  uint32_t divisor = 1193182 / frequency;
  outb(0x43, 0x36);
  outb(0x40, (uint8_t)(divisor & 0xFF));
  outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

extern uint32_t target_fps;

void isr_handler(struct registers *r) {
  __asm__ volatile("cli");

  int old_shell_x = task_list[0].cursor_x;
  int old_shell_y = task_list[0].cursor_y;

  VESA_draw_rect(0, 0, 1024, 768, 0xAA0000);
  task_list[0].cursor_x = 10;
  task_list[0].cursor_y = 10;
  VESA_print("===================================================\n", 0xFFFFFF);
  VESA_print("             KDXOS EXCEPTION RECOVERY              \n", 0xFFFFFF);
  VESA_print("===================================================\n\n",
             0xFFFFFF);

  char log_entry[256];
  log_entry[0] = '\0';
  kstrcat(log_entry, "\n[CRASH] Task: ");
  itoa(current_task_idx, log_entry + kstrlen(log_entry), 10);
  kstrcat(log_entry, " | Exception: ");
  kstrcat(log_entry, exception_messages[r->int_no]);
  kstrcat(log_entry, " | EIP: 0x");
  char hex_addr[16];
  itoa(r->eip, hex_addr, 16);
  kstrcat(log_entry, hex_addr);
  kstrcat(log_entry, "\n");
  klog_append(log_entry);

  VESA_print("EXCEPTION: ", 0xFFFFFF);
  VESA_print(exception_messages[r->int_no], 0xFFFF00);
  VESA_print("\nEIP: 0x", 0xAAAAAA);
  VESA_print(hex_addr, 0xFFFFFF);

  VESA_flip();

  int id = current_task_idx;
  if (id > 0) {
    for (volatile uint32_t i = 0; i < 300000000; i++)
      ;

    task_list[0].cursor_x = old_shell_x;
    task_list[0].cursor_y = old_shell_y;

    task_list[id].state = 0;
    if (task_list[id].window_buffer) {
      kfree(task_list[id].window_buffer);
      task_list[id].window_buffer = NULL;
    }

    refresh_tiling_layout();

    for (int i = 0; i < MAX_TASKS; i++) {
      if (task_list[i].state != 0) {
        mark_task_dirty(i, 0, 0, 4000, 4000);
      }
    }

    VESA_flip();

    int next = 0;
    current_task_idx = next;
    uint32_t new_esp = task_list[next].esp;

    __asm__ volatile("mov %0, %%esp \n"
                     "pop %%eax     \n"
                     "mov %%ax, %%ds\n"
                     "mov %%ax, %%es\n"
                     "popa          \n"
                     "add $8, %%esp \n"
                     "sti           \n"
                     "iret          \n"
                     :
                     : "r"(new_esp));

  } else {
    VESA_print("\n\nCRITICAL SYSTEM FAILURE. HALTING.", 0xFFFFFF);
    VESA_flip();
    while (1)
      __asm__ volatile("hlt");
  }
}

uint32_t timer_handler(struct registers *regs) {
  system_ticks++;
  outb(0x20, 0x20); // ACK the interrupt to the PIC early

  // Let the central scheduler decide what happens next
  return schedule_next((uint32_t)regs);
}
int is_extended = 0;

#define KBD_UP 11
#define KBD_DOWN 12
#define KBD_LEFT 13
#define KBD_RIGHT 14

uint32_t keyboard_handler(struct registers *regs) {
  uint8_t scancode = inb(0x60);

  if (scancode == 0xE0) {
    is_extended = 1;
    outb(0x20, 0x20);
    return (uint32_t)regs;
  }

  if (scancode & 0x80) {
    uint8_t released = scancode & 0x7F;
    if (released == 0x1D)
      ctrl_state = 0;
    else if (released == 0x2A || released == 0x36)
      shift_state = 0;
    else if (released == 0x38) {
      if (is_extended)
        altgr_state = 0;
      else
        alt_state = 0;
    }
    is_extended = 0;
    outb(0x20, 0x20);
    return (uint32_t)regs;
  }

  char c = 0;
  if (is_extended) {
    if (scancode == 0x48)
      c = KBD_UP;
    else if (scancode == 0x50)
      c = KBD_DOWN;
    else if (scancode == 0x4B)
      c = KBD_LEFT;
    else if (scancode == 0x4D)
      c = KBD_RIGHT;
    else if (scancode == 0x38)
      altgr_state = 1;
    is_extended = 0;
  } else {
    if (scancode == 0x1D)
      ctrl_state = 1;
    else if (scancode == 0x2A || scancode == 0x36)
      shift_state = 1;
    else if (scancode == 0x38)
      alt_state = 1;
    else if (scancode == 0x3B)
      keyboard_focus_tid = 0;

    // --- HOTKEY: CTRL + TAB (Cycle Focus) ---
    else if (scancode == 0x0F && ctrl_state) {
      int start = keyboard_focus_tid;
      int next = (start + 1) % MAX_TASKS;

      while (next != start) {
        if (task_list[next].state != 0 && task_list[next].has_window) {
          mark_task_dirty(keyboard_focus_tid, 0, 0, 4000, 4000);
          keyboard_focus_tid = next;
          mark_task_dirty(keyboard_focus_tid, 0, 0, 4000, 4000);

          if (task_list[next].state == 3)
            task_list[next].state = 1;
          break;
        }
        next = (next + 1) % MAX_TASKS;
      }
    } else if (scancode == 0x1C && alt_state) {
      extern int pending_shell_spawn;
      pending_shell_spawn = 1;
    } else {
      c = scancode_to_ascii(scancode, shift_state, altgr_state);
    }
  }

  if (c != 0) {
    if (ctrl_state && c >= 'a' && c <= 'z')
      c = c - 'a' + 1;

    volatile struct task *target = &task_list[keyboard_focus_tid];
    int next = (target->kbd_head + 1) % 64;
    if (next != target->kbd_tail) {
      target->kbd_buffer[target->kbd_head] = c;
      target->kbd_head = next;

      // THE FIX: Immediately switch to the task waiting for input
      if (target->state == 3) {
        target->state = 1;
        task_list[current_task_idx].esp = (uint32_t)regs;
        current_task_idx = keyboard_focus_tid;
        outb(0x20, 0x20); // ACK early
        return task_list[current_task_idx].esp;
      }
    }
  }

  outb(0x20, 0x20);
  return (uint32_t)regs; // Return unchanged ESP if no task switch occurred
}
void emit_mov(uint8_t reg_code, uint32_t val, uint8_t *out_buf, uint32_t *pos) {
  if (out_buf) {
    out_buf[*pos] = reg_code;
  }
  (*pos)++;
  if (out_buf) {
    kmemcpy(&out_buf[*pos], &val, 4);
  }
  *pos += 4;
}

void emit_load(uint8_t reg_opcode, const char *arg, uint8_t *out_buf,
               uint32_t *pos) {
  if ((arg[0] >= '0' && arg[0] <= '9') || arg[0] == '-') {
    if (out_buf) {
      uint32_t val = (arg[0] == '0' && arg[1] == 'x')
                         ? katoh(arg)
                         : (uint32_t)katoi((char *)arg);
      out_buf[*pos] = reg_opcode;
      kmemcpy(&out_buf[*pos + 1], &val, 4);
    }
    *pos += 5;
  } else {
    if (out_buf) {
      uint32_t offset = get_var_offset(arg);
      out_buf[*pos] = 0x8B;
      if (reg_opcode == 0xB8)
        out_buf[*pos + 1] = 0x05;
      else if (reg_opcode == 0xBB)
        out_buf[*pos + 1] = 0x1D;
      else if (reg_opcode == 0xB9)
        out_buf[*pos + 1] = 0x0D;
      else if (reg_opcode == 0xBA)
        out_buf[*pos + 1] = 0x15;
      else if (reg_opcode == 0xBE)
        out_buf[*pos + 1] = 0x35;
      else if (reg_opcode == 0xBF)
        out_buf[*pos + 1] = 0x3D;
      kmemcpy(&out_buf[*pos + 2], &offset, 4);
    }
    *pos += 6;
  }
}

uint32_t syscall_handler(struct registers *regs) {
  __asm__ volatile("sti");

  struct multiboot_info *mbi = VESA_get_boot_info();
  uint32_t sw = mbi->framebuffer_width;
  int id = current_task_idx;
  int border = WIN_BORDER;

  if (regs->eax == 1) { // DRAW_CHAR
    char c = (char)regs->ebx;
    int x = regs->ecx + border;
    int y = regs->edx + border;

    if (task_list[id].has_window && task_list[id].window_buffer) {
      extern uint8_t font8x8_basic[128][8];
      uint8_t *glyph = font8x8_basic[(unsigned char)c];

      for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
          if (glyph[row] & (1 << (7 - col))) {
            int draw_x = x + col;
            int draw_y = y + row;

            if (draw_x >= 0 && draw_x < task_list[id].win_w && draw_y >= 0 &&
                draw_y < task_list[id].win_h) {
              task_list[id].window_buffer[(draw_y * sw) + draw_x] = 0xFFFFFF;
            }
          }
        }
      }
      mark_task_dirty(id, x, y, 8, 8);
    }
  } else if (regs->eax == 2) { // GET_TICKS
    regs->eax = system_ticks;
  } else if (regs->eax == 3) { // SLEEP
    uint32_t ms = regs->ebx;
    task_list[id].sleep_ticks = (ms * timer_frequency) / 1000;
    if (task_list[id].sleep_ticks == 0)
      task_list[id].sleep_ticks = 1;

    task_list[id].state = 2; // Mark as sleeping

    // FIX: Yield the CPU immediately instead of getting stuck in a while loop!
    return schedule_next((uint32_t)regs);
  } else if (regs->eax == 4) { // EXIT
    int id = current_task_idx;

    // Stop blitting this task
    task_list[id].window_ready = 0;
    task_list[id].has_window = 0;
    task_list[id].state = 0; // DEAD

    if (task_list[id].window_buffer) {
      kfree(task_list[id].window_buffer);
      task_list[id].window_buffer = NULL;
    }

    refresh_tiling_layout();

    // If this was the focused task, pass focus to someone else
    if (keyboard_focus_tid == id) {
      keyboard_focus_tid = 0; // Default to kernel/shell
    }

    // Force everyone else to redraw their borders/contents over the gap
    for (int i = 0; i < MAX_TASKS; i++) {
      if (task_list[i].state != 0)
        mark_task_dirty(i, 0, 0, 4000, 4000);
    }

    regs->eip = (uint32_t)idle_task_code;
    return schedule_next((uint32_t)regs);
  } else if (regs->eax == 5) { // CLEAR WINDOW
    if (task_list[id].has_window && task_list[id].window_buffer) {
      uint32_t *buf = task_list[id].window_buffer;
      uint32_t count = sw * task_list[id].win_h;
      uint32_t color = 0x222222; // Your standard background color

      // Use 'rep stosl' for a professional, high-speed memory fill
      __asm__ volatile("rep stosl"
                       : "+D"(buf), "+c"(count)
                       : "a"(color)
                       : "memory");

      // Mark the entire window as dirty so the WM redraws it
      mark_task_dirty(id, 0, 0, task_list[id].win_w, task_list[id].win_h);
    }
  } else if (regs->eax == 6) { // DRAW_RECT
    int rx = regs->ebx + border;
    int ry = regs->ecx + border;
    int rw = regs->edx;
    int rh = regs->esi;
    uint32_t color = regs->edi;

    if (task_list[id].has_window && task_list[id].window_buffer) {
      for (int iy = 0; iy < rh; iy++) {
        for (int ix = 0; ix < rw; ix++) {
          int dx = rx + ix;
          int dy = ry + iy;
          if (dx >= 0 && dx < task_list[id].win_w && dy >= 0 &&
              dy < task_list[id].win_h) {
            task_list[id].window_buffer[(dy * sw) + dx] = color;
          }
        }
      }
      mark_task_dirty(id, rx, ry, rw, rh);
    }
  } else if (regs->eax == 7) { // PRINT_STR
    uint32_t aligned_code =
        ((uint32_t)task_list[id].code_ptr + 0xFFF) & 0xFFFFF000;
    char *str = (char *)(regs->ebx + aligned_code);
    uint32_t color = regs->edi;

    // CHECK FOR GUI WINDOW
    if (task_list[id].has_window && task_list[id].window_buffer) {
      int sx = regs->ecx + border;
      int sy = regs->edx + border;

      int chars_printed = 0;
      for (int i = 0; str[i] != '\0'; i++) {
        int cur_x = sx + (i * 8);
        unsigned char c = (unsigned char)str[i];
        if (c > 127)
          c = '?';

        extern uint8_t font8x8_basic[128][8];
        uint8_t *glyph = font8x8_basic[c];

        for (int row = 0; row < 8; row++) {
          for (int col = 0; col < 8; col++) {
            if (glyph[row] & (1 << (7 - col))) {
              int dx = cur_x + col;
              int dy = sy + row;
              if (dx >= 0 && dx < task_list[id].win_w && dy >= 0 &&
                  dy < task_list[id].win_h) {
                task_list[id].window_buffer[(dy * sw) + dx] = color;
              }
            }
          }
        }
        chars_printed++;
      }
      mark_task_dirty(id, sx, sy, chars_printed * 8, 8);
    }
    // HEADLESS FALLBACK (STDOUT)
    else {
      extern void shell_print(char *str, uint32_t color);
      shell_print(str, color);
    }
  } else if (regs->eax == 8) { // CREATE_WINDOW
    task_list[id].has_window = 1;
    if (task_list[id].window_buffer == NULL) {
      uint32_t sh = mbi->framebuffer_height;
      task_list[id].window_buffer = (uint32_t *)kmalloc(sw * sh * 4);
      if (!task_list[id].window_buffer) {
        kprintf_unsync("OOM: Window failed\n");
        task_list[id].has_window = 0;
        return (uint32_t)regs;
      }
      for (uint32_t i = 0; i < (sw * sh); i++)
        task_list[id].window_buffer[i] = 0x222222;
    }
    task_list[id].cursor_x = 0;
    task_list[id].cursor_y = 0;
    refresh_tiling_layout();

    mark_task_dirty(id, 0, 0, sw, mbi->framebuffer_height);
  } else if (regs->eax == 9) {       // PRINT_NUM (Value is now directly in EBX)
    uint32_t actual_val = regs->ebx; // Use the value directly!
    uint32_t color = regs->edi;      // Moved color up here for scope access

    char buf[16];
    itoa(actual_val, buf, 10);

    // CHECK FOR GUI WINDOW
    if (task_list[id].has_window && task_list[id].window_buffer) {
      int x = regs->ecx + border;
      int y = regs->edx + border;

      for (int i = 0; buf[i] != '\0'; i++) {
        extern uint8_t font8x8_basic[128][8];
        uint8_t *glyph = font8x8_basic[(unsigned char)buf[i]];
        int cur_x = x + (i * 8);

        for (int row = 0; row < 8; row++) {
          for (int col = 0; col < 8; col++) {
            if (glyph[row] & (1 << (7 - col))) {
              int dx = cur_x + col;
              int dy = y + row;
              if (dx >= 0 && dx < task_list[id].win_w && dy >= 0 &&
                  dy < task_list[id].win_h) {
                task_list[id].window_buffer[(dy * sw) + dx] = color;
              }
            }
          }
        }
      }
      mark_task_dirty(id, x, y, kstrlen(buf) * 8, 8);
    }
    // HEADLESS FALLBACK (STDOUT)
    else {
      extern void shell_print(char *str, uint32_t color);
      shell_print(buf, color);
    }
  }
  // --- THE SYSCALL 10 FIX ---
  else if (regs->eax == 10) {
    volatile struct task *t = &task_list[id];

    // Direct memory read. Bypasses the old broken functions!
    if (t->kbd_head != t->kbd_tail) {
      char c = t->kbd_buffer[t->kbd_tail];
      t->kbd_tail = (t->kbd_tail + 1) % 64;
      regs->eax = (uint32_t)c;
    } else {
      regs->eax = 0; // Return 0 if no key is pressed
    }
    return (uint32_t)regs;
  } else if (regs->eax == 11) { // VFS_LOAD_FILE
    uint32_t aligned_code =
        ((uint32_t)task_list[id].code_ptr + 0xFFF) & 0xFFFFF000;
    char *path = (char *)(regs->ebx + aligned_code);

    // Use the VFS Walker instead of fat_search
    vfs_node_t *node = vfs_walk_path(path, NULL);

    if (node) {
      void *buffer = kmalloc(node->size + 512); // Extra padding for safety
      uint32_t bytes_read = vfs_read(node, 0, node->size, (uint8_t *)buffer);

      regs->eax = (uint32_t)buffer;
      regs->ecx = bytes_read; // The size of the virtual data

      kfree(node); // Clean up the VFS node
    } else {
      regs->eax = 0;
      regs->ecx = 0;
    }
  } else if (regs->eax == 12) { // FREE_MEM
    // EBX holds the pointer we want to free
    void *ptr = (void *)regs->ebx;
    if (ptr != NULL && ptr != 0) {
      kfree(ptr);
    }
  } else if (regs->eax == 13) { // DRAW_CHAR_COLORED
    char c = (char)(regs->ebx & 0xFF);
    uint32_t color = regs->edi;

    // CHECK FOR GUI WINDOW
    if (task_list[id].has_window && task_list[id].window_buffer) {
      int x = regs->ecx + border;
      int y = regs->edx + border;
      extern uint8_t font8x8_basic[128][8];
      uint8_t *glyph = font8x8_basic[(unsigned char)c];

      for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
          if (glyph[row] & (1 << (7 - col))) {
            int dx = x + col, dy = y + row;
            if (dx >= 0 && dx < task_list[id].win_w && dy >= 0 &&
                dy < task_list[id].win_h)
              task_list[id].window_buffer[(dy * sw) + dx] = color;
          }
        }
      }
      mark_task_dirty(id, x, y, 8, 8);
    }
    // HEADLESS FALLBACK (STDOUT)
    else {
      // Convert the single char into a null-terminated string for shell_print
      char temp_str[2] = {c, '\0'};
      extern void shell_print(char *str, uint32_t color);
      shell_print(temp_str, color);
    }
  } else if (regs->eax == 14) { // GET_ARGC
    // Calculate the task's base memory exactly like Syscall 7 does
    uint32_t aligned_code =
        ((uint32_t)task_list[id].code_ptr + 0xFFF) & 0xFFFFF000;

    // Read the injected value and return it in EAX
    uint32_t *argc_ptr = (uint32_t *)(aligned_code + 2800);
    regs->eax = *argc_ptr;
  }
  return (uint32_t)regs;
}
