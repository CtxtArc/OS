#include <stddef.h>

void itoa(int n, char* str, int base);
int katoi(char* str);
const char* get_token(const char* line, char* token_out); 
uint32_t katoh(const char* s); 

void kprintf(const char* format, ...);
void kprintf_color(int color, const char* format, ...);
void kprintf_unsync(const char* format, ...); 

int kstrcmp(const char* a, const char* b); 
int kstrncmp(const char* s1, const char* s2, size_t n);
int kstrcasecmp(const char* s1, const char* s2); 

char* kstrcpy(char* dest, const char* src);
char* kstrncpy(char* dest, const char* src, size_t n);

size_t kstrlen(const char* str);


uint32_t parse_literal(const char* s);
char* kstrcat(char* dest, const char* src);
void hexdump(void* ptr, int size); 
void sleep(int ms); 
void klog_append(const char* msg); 
