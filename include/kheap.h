#ifndef KHEAP
#define KHEAP
#include <stdint.h>
#include <stddef.h>

// Every block of memory on the heap starts with this header
typedef struct header {
    uint32_t size;   // Size of the block (excluding this header)
    uint32_t  is_free; // 1 if the block can be reused, 0 if it's taken
    struct header* next;
} __attribute__((packed)) header_t;

void init_kheap();
void* kmalloc(uint32_t size);
void* kmalloc_a(uint32_t size); 
void kfree(void* ptr);
void* kmemcpy(void* dest, const void* src, uint32_t n);
void* kmemcpy32(void* dest, const void* src, uint32_t n);
void* kmemset(void* dest, uint32_t val, uint32_t n);
void kheap_stats();
void* kmemset(void* dest, uint32_t val, uint32_t n);
void kheap_dump_map();
void kheap_stats();
#endif // !KHEAP
