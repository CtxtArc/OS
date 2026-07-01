#ifndef IDT
#define IDT

#include <stdint.h>
#define SYS_DRAW_CHAR 1
#define SYS_GET_TICKS 2
#define SYS_SLEEP 3
#define TIMER_HZ 1000
// For the Assembler, we would define these as constants:
// .define SYS_DRAW_CHAR 1
extern uint32_t timer_frequency; // Global variable to store the frequency
struct idt_entry {
  uint16_t base_low;  // Lower 16 bits of handler function address
  uint16_t sel;       // Kernel segment selector
  uint8_t always0;    // This must always be zero
  uint8_t flags;      // Flags (Presence, Privilege, Type)
  uint16_t base_high; // Upper 16 bits of handler function address
} __attribute__((packed));

struct idt_ptr {
  uint16_t limit;
  uint32_t base;
} __attribute__((packed));

struct registers {
  uint32_t ds; // Data segment (pushed last)
  uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax; // Pushed by PUSHA
  uint32_t int_no, err_code;             // Pushed by assembly stub
  uint32_t eip, cs, eflags, useresp, ss; // Pushed by CPU
};
void idt_init();
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags);
void pic_remap();
void idt_init();
uint32_t timer_handler(struct registers *regs);
void timer_init(uint32_t frequency);
uint32_t keyboard_handler(struct registers *regs);
uint32_t syscall_handler(struct registers *regs);
void emit_load(uint8_t reg_opcode, const char *arg, uint8_t *out_buf,
               uint32_t *pos);
void emit_mov(uint8_t reg_code, uint32_t val, uint8_t *out_buf, uint32_t *pos);
#endif // !IDT
