#include "vesa.h"
#include <stdint.h>

void paging_init(struct multiboot_info *mbi);
void map_page(uint32_t virtual_addr, uint32_t physical_addr);
void flush_tlb();
void make_user_accessible(uint32_t virt, uint32_t size);
