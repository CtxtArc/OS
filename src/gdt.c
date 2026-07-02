#include "gdt.h"
#include "kheap.h" // For kmemset

struct gdt_entry gdt[6]; // Increased from 3 to 6
struct gdt_ptr gp;
struct tss_entry_struct tss_entry;

extern void gdt_flush(uint32_t);
extern void tss_flush(); // Assembly function to load the TSS

void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access,
                  uint8_t gran) {
  gdt[num].base_low = (base & 0xFFFF);
  gdt[num].base_middle = (base >> 16) & 0xFF;
  gdt[num].base_high = (base >> 24) & 0xFF;
  gdt[num].limit_low = (limit & 0xFFFF);
  gdt[num].granularity = (limit >> 16) & 0x0F;
  gdt[num].granularity |= gran & 0xF0;
  gdt[num].access = access;
}

void write_tss(int32_t num, uint16_t ss0, uint32_t esp0) {
  uint32_t base = (uint32_t)&tss_entry;
  uint32_t limit = sizeof(tss_entry);

  // Add the TSS descriptor to the GDT (0xE9 = Present, Executable, Accessed)
  gdt_set_gate(num, base, limit, 0xE9, 0x00);

  // Ensure the TSS is initially zeroed
  kmemset(&tss_entry, 0, sizeof(tss_entry));

  tss_entry.ss0 = ss0;   // Set the kernel stack segment (0x10)
  tss_entry.esp0 = esp0; // Set the kernel stack pointer

  // Set the IO map base to the end of the TSS (disables user IO port access)
  tss_entry.iomap_base = sizeof(tss_entry);
}

// The scheduler will call this when switching tasks to ensure
// the CPU knows where to dump registers if an interrupt fires in Ring 3
void set_kernel_stack(uint32_t stack) { tss_entry.esp0 = stack; }

void gdt_init() {
  gp.limit = (sizeof(struct gdt_entry) * 6) - 1;
  gp.base = (uint32_t)&gdt;

  gdt_set_gate(0, 0, 0, 0, 0);                // 0x00: Null segment
  gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF); // 0x08: Kernel Code
  gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF); // 0x10: Kernel Data

  // User Segments: DPL is set to 3 (Ring 3)
  // 0xFA = 11111010 (Present, Ring 3, Code)
  // 0xF2 = 11110010 (Present, Ring 3, Data)
  gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF); // 0x18: User Code
  gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF); // 0x20: User Data

  // Initialize the TSS at entry 5 (0x28). 0x10 is your Kernel Data Segment.
  write_tss(5, 0x10, 0x0);

  gdt_flush((uint32_t)&gp);
  tss_flush(); // Tell the CPU to start using the TSS
}
