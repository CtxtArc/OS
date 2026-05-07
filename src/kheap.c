#include "kheap.h"
#include "lib.h"

// Set the heap to 64MB so we can easily hold multiple 1024x768x4 window buffers
#define HEAP_INITIAL_SIZE (64 * 1024 * 1024) 

// The Linker provides this symbol
extern uint32_t end; 
uint32_t placement_address = (uint32_t)&end;

header_t* heap_start = NULL;

void init_kheap() {
    uint32_t start_node = 0x800000; // Heap starts at the 8MB mark in physical RAM
    uint32_t total_heap_size = HEAP_INITIAL_SIZE; 

    heap_start = (header_t*)start_node;
    heap_start->size = total_heap_size - sizeof(header_t); 
    heap_start->is_free = 1;
    heap_start->next = NULL;
}

void kfree(void* ptr) {
    if (!ptr) return;

    header_t* curr = heap_start;
    header_t* prev = NULL;
    header_t* target = NULL;
    header_t* prev_save = NULL;

    // 1. SEARCH: Find the actual header managing this pointer
    // This handles aligned pointers from kmalloc_a safely
    while (curr) {
        uint32_t data_start = (uint32_t)curr + sizeof(header_t);
        uint32_t data_end = data_start + curr->size;

        if ((uint32_t)ptr >= data_start && (uint32_t)ptr < data_end) {
            target = curr;
            prev_save = prev;
            break;
        }
        prev = curr;
        curr = curr->next;
    }

    if (!target) return; // Pointer not found in heap

    // 2. MARK AS FREE
    target->is_free = 1;

    // 3. COALESCE FORWARD: Merge with next if free
    if (target->next && target->next->is_free) {
        target->size += sizeof(header_t) + target->next->size;
        target->next = target->next->next;
    }

    // 4. COALESCE BACKWARD: Merge with previous if free
    // This is the "magic" that turns tiny holes back into a massive block
    if (prev_save && prev_save->is_free) {
        prev_save->size += sizeof(header_t) + target->size;
        prev_save->next = target->next;
    }
}

void* kmemcpy(void* dest, const void* src, uint32_t n) {
    __asm__ volatile (
        "movl %0, %%edi\n"
        "movl %1, %%esi\n"
        "movl %2, %%ecx\n"
        "rep movsb"
        : 
        : "r"(dest), "r"(src), "r"(n)
        : "edi", "esi", "ecx", "memory"
    );
    return dest;
}

// Use this for Graphics (VESA_flip, VESA_scroll)
void* kmemcpy32(void* dest, const void* src, uint32_t n) {
    __asm__ volatile (
        "rep movsl"
        : "+D"(dest), "+S"(src), "+c"(n)
        : : "memory"
    );
    return dest;
}

void kheap_stats() {
    uint32_t free_mem = 0; 
    uint32_t used_mem = 0;
    uint32_t blocks = 0;
    
    header_t* curr = heap_start;
    // Updated to use the HEAP_INITIAL_SIZE macro!
    uint32_t heap_limit = (uint32_t)heap_start + HEAP_INITIAL_SIZE; 
    kprintf("Scanning Heap at 0x%x...\n", (uint32_t)heap_start);

    while (curr != NULL) {
        // --- THE SAFETY CHECK ---
        // If curr is outside the pool, the list is broken. 
        if ((uint32_t)curr < (uint32_t)heap_start || (uint32_t)curr >= heap_limit) {
            kprintf("Error: Heap linked-list corrupted at 0x%x\n", (uint32_t)curr);
            break;
        }

        if (curr->is_free) {
            free_mem += curr->size;
        } else {
            used_mem += curr->size;
        }
        
        blocks++;

        // Safety: If size is 0, we will loop forever. Stop that.
        if (curr->size == 0 && curr->next != NULL) {
            kprintf("Error: Zero-size block detected.\n");
            break;
        }

        curr = curr->next;
    }

    kprintf("Blocks: %d | Used: %d | Free: %d\n", blocks, used_mem, free_mem);
}

void* kmalloc(uint32_t size) {
    if (size == 0) return NULL;

    // 1. ALIGNMENT: 4-byte boundaries are non-negotiable for heap stability
    size = (size + 3) & ~3;

    header_t* curr = heap_start;

    while (curr) {
        if (curr->is_free && curr->size >= size) {
            
            // 2. THE HEALER SPLIT LOGIC
            // We only split if we have enough room for:
            // The requested data + current header + NEW header + at least 16 bytes of data.
            uint32_t total_needed_to_split = size + sizeof(header_t) + 16;

            if (curr->size >= total_needed_to_split) {
                // Calculate exactly where the NEW header will live
                header_t* next_block = (header_t*)((uint32_t)curr + sizeof(header_t) + size);
                
                // The remainder is the current size minus what we took and the new header
                next_block->size = curr->size - size - sizeof(header_t);
                next_block->is_free = 1;
                
                // Link the new block to whatever the current block was pointing to
                next_block->next = curr->next;

                // Shrink the current block to the exact size requested
                curr->size = size;
                
                // Point current to the new block, effectively inserting it into the list
                curr->next = next_block;
            } else {
                // INTERNAL FRAGMENTATION:
                // We don't split. curr->size remains larger than requested.
            }
            
            // 3. LOCK AND RETURN
            curr->is_free = 0;
            return (void*)((uint32_t)curr + sizeof(header_t));
        }
        
        curr = curr->next;
    }

    return NULL; // Truly out of memory
}

void* kmalloc_a(uint32_t size) {
    // 1. We allocate enough extra space to find an aligned spot 
    // without ever moving the actual header.
    uint32_t total_needed = size + 4096; 
    void* ptr = kmalloc(total_needed);
    if (!ptr) return NULL;

    uint32_t addr = (uint32_t)ptr;
    
    // 2. If it's already aligned, just return it
    if (!(addr & 0xFFF)) return ptr;

    // 3. Otherwise, find the next aligned address WITHIN the block we just got
    uint32_t aligned_addr = (addr + 0xFFF) & 0xFFFFF000;
    
    return (void*)aligned_addr;
}

void* kmemset(void* dest, uint32_t val, uint32_t n) {
    __asm__ volatile (
        "rep stosl"
        : "+D"(dest), "+c"(n)
        : "a"(val)
        : "memory"
    );
    return dest;
}

void kheap_dump_map() {
    header_t* curr = heap_start;
    int i = 0;
    uint32_t total_calculated = 0;

    kprintf_unsync("\n--- HEAP MAP DEBUG ---\n");
    while (curr) {
        uint32_t data_start = (uint32_t)curr + sizeof(header_t);
        
        kprintf_unsync("B%d: [%s] Addr: 0x%x | Data: 0x%x | Size: %d | Next: 0x%x\n", 
            i++, 
            curr->is_free ? "FREE" : "USED", 
            (uint32_t)curr, 
            data_start, 
            curr->size, 
            (uint32_t)curr->next
        );

        total_calculated += sizeof(header_t) + curr->size;

        if (i > 20) { // Safety break
            kprintf_unsync("... stopping dump after 20 blocks ...\n");
            break;
        }
        curr = curr->next;
    }
    kprintf_unsync("Total Heap Coverage: %d bytes\n", total_calculated);
    kprintf_unsync("----------------------\n");
}
