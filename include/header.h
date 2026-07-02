#ifndef HEADER_H
#define HEADER_H
#include <stdint.h>

typedef struct {
  uint32_t magic;       // 0x4B445821 ("KDX!")
  uint32_t version;     // 1
  uint32_t entry_point; // Offset where the code starts
  uint32_t heap_size;   // How much memory the app needs
  uint32_t flags;       // 0: Console/Headless, 1: GUI Windowed
} __attribute__((packed)) kdx_header_t;

#endif
