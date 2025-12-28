#ifndef ASSEM_H
#define ASSEM_H

#include <stdint.h>

typedef struct {
    char name[32];
    uint32_t address;
} Label;

typedef struct {
    char name[32];
    uint32_t offset; // This tells us where in the 4KB buffer the variable lives
} RuntimeVar;

extern RuntimeVar rt_vars[64];
extern int rt_var_count;
extern Label label_table[64];
extern int label_count;

void assemble_line(const char* line, uint8_t* out_buf, uint32_t* pos, int pass); 
uint32_t get_var_offset(const char* name); 

#endif // !ASSEM_H
