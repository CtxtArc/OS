#ifndef GDT
#define GDT
#include <stdint.h>

struct gdt_entry {
  uint16_t limit_low;
  uint16_t base_low;
  uint8_t base_middle;
  uint8_t access;
  uint8_t granularity;
  uint8_t base_high;
} __attribute__((packed));

struct gdt_ptr {
  uint16_t limit;
  uint32_t base;
} __attribute__((packed));

// The TSS structure (Hardware relies on this exact layout)
struct tss_entry_struct {
  uint32_t prev_tss;
  uint32_t esp0; // Kernel stack pointer
  uint32_t ss0;  // Kernel stack segment
  uint32_t esp1, ss1, esp2, ss2, cr3, eip, eflags;
  uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
  uint32_t es, cs, ss, ds, fs, gs;
  uint32_t ldt;
  uint16_t trap;
  uint16_t iomap_base;
} __attribute__((packed));

void gdt_init();
void set_kernel_stack(uint32_t stack);

#endif // !GDT
