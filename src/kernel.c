#include "shell.h"
#include "io.h"
#include "lib.h"
#include "idt.h"
#include "pmm.h"
#include "gdt.h"
#include "task.h"
#include "paging.h"
#include "kheap.h"
#include "vesa.h"
#include "fat.h"

// External references for memory and info
extern char end;
extern int system_ticks;
extern void fat_vfs_mount();

void kmain(uint32_t magic, struct multiboot_info* mbi) {
    system_ticks = 0;
    if (!(mbi->flags & (1 << 12))) return; 

    // 1. Core CPU structures
    gdt_init(); 
    idt_init();       
    pic_remap();      // Remap PIC before any hardware init

    // 2. Memory Management (Critical Order)
    uint32_t ram_kb = mbi->mem_upper + 1024;
    pmm_init(ram_kb * 1024, (uint32_t)&end); // PMM first
    paging_init(mbi);                        // Paging second
    init_kheap();                            // Heap third
  

    // 3. Hardware / Graphics
    VESA_init(mbi);
    timer_init(1000);  

    // 4. Filesystem & Tasks
    fat_init();      // Sets up the hard drive variables
    fat_vfs_mount(); // Mounts the hard drive to the VFS '/' root!
    run_startup_tests();
    init_multitasking(); 

    // 5. Final output and interrupts
    __asm__ volatile("sti");
    
while(1) {
        __asm__ volatile("hlt"); // Put the CPU in a low-power state until the next timer tick
    }
}
