#include <stdint.h>

typedef struct {
    char name[32];
    uint32_t address;
} Label;

extern Label label_table[64];
extern int label_count;

void assemble_line(const char* line, uint8_t* out_buf, uint32_t* pos, int pass); 
