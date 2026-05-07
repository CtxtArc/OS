#include "paging.h"
#include "vesa.h"
#include <stdint.h>

// A page directory entry
uint32_t page_directory[1024] __attribute__((aligned(4096)));
uint32_t vesa_page_tables[2][1024] __attribute__((aligned(4096)));

// FIX: We need 32 page tables to cover 128MB of RAM (4MB per table).
// This guarantees our 64MB Heap is fully mapped into physical memory!
uint32_t kernel_page_tables[32][1024] __attribute__((aligned(4096)));

extern void load_page_directory(unsigned int*);
extern void enable_paging();

void map_page(uint32_t virtual_addr, uint32_t physical_addr) {
    uint32_t dir_index = virtual_addr >> 22;
    uint32_t table_index = (virtual_addr >> 12) & 0x03FF;

    if (dir_index < 32) { // Updated to 32
        kernel_page_tables[dir_index][table_index] = (physical_addr & ~0xFFF) | 3;
        flush_tlb();
    }
}

void flush_tlb() {
    uint32_t reg;
    __asm__ volatile("mov %%cr3, %0" : "=r"(reg));
    __asm__ volatile("mov %0, %%cr3" : : "r"(reg));
}

void paging_init(struct multiboot_info* mbi) {
    // 1. Clear Page Directory
    for(int i = 0; i < 1024; i++) {
        page_directory[i] = 0x00000002; // Not present, writable
    }

    // 2. Identity Map the first 128MB of RAM
    for (int t = 0; t < 32; t++) { // 32 tables * 4MB = 128MB mapped!
        for (int i = 0; i < 1024; i++) {
            uint32_t phys = (t * 4 * 1024 * 1024) + (i * 4096);
            kernel_page_tables[t][i] = phys | 3; // Present + Writable
        }
        page_directory[t] = ((uint32_t)kernel_page_tables[t]) | 3;
    }

    // 3. Identity Map the VESA Framebuffer (Hardware VRAM)
    uint32_t fb_phys = (uint32_t)mbi->framebuffer_addr;
    uint32_t dir_idx_start = fb_phys >> 22;
    
    // Map exactly what we need (2 tables = 8MB coverage)
    for (int t = 0; t < 2; t++) {
        for (int i = 0; i < 1024; i++) {
            uint32_t phys_offset = (t * 1024 * 4096) + (i * 4096);
            uint32_t actual_phys = fb_phys + phys_offset;
            vesa_page_tables[t][i] = (actual_phys & ~0xFFF) | 3;
        }
        
        if ((dir_idx_start + t) < 1024) {
            page_directory[dir_idx_start + t] = ((uint32_t)vesa_page_tables[t]) | 3;
        }
    }
    
    // 4. Load and Enable
    load_page_directory(page_directory);
    enable_paging();

    __asm__ volatile("sti"); 
}
