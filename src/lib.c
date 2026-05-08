#include "vesa.h"
#include <stdarg.h>
#include "lib.h"
#include <stddef.h>

#define KLOG_BUF_SIZE 4096


extern int timer_frequency;

 char klog_ram_buffer[KLOG_BUF_SIZE];
volatile int klog_needs_sync = 0; 

void klog_append(const char* msg) {
    // Simple append logic for the RAM buffer
    static uint32_t p = 0;
    for (int i = 0; msg[i] != '\0'; i++) {
        klog_ram_buffer[p++] = msg[i];
        if (p >= KLOG_BUF_SIZE - 1) p = 0; // Wrap around
    }
    klog_ram_buffer[p] = '\0';
    klog_needs_sync = 1; 
}
int kstrcmp(const char* a, const char* b) {
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return *(unsigned char*)a - *(unsigned char*)b;
}

int katoi(char* str) {
    int res = 0;
    for (int i = 0; str[i] != '\0'; ++i) {
        if (str[i] < '0' || str[i] > '9') break;
        res = res * 10 + str[i] - '0';
    }
    return res;
}

char* kstrcpy(char* dest, const char* src) {
    char* saved = dest;
    while ((*dest++ = *src++) != '\0');
    return saved;
}

int kstrncmp(const char* s1, const char* s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        // If characters are different, or we hit the end of s1
        if (s1[i] != s2[i] || s1[i] == '\0') {
            return (unsigned char)s1[i] - (unsigned char)s2[i];
        }
    }
    // If we checked n characters and they were all the same
    return 0;
}

char* kstrncpy(char* dest, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for ( ; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

size_t kstrlen(const char* str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}
void kprintf_unsync(const char* format, ...) {
    va_list args;
    va_start(args, format);

    for (int i = 0; format[i] != '\0'; i++) {
        if (format[i] == '%') {
            i++;
            switch (format[i]) {
                case 'c': {
                    char c = (char)va_arg(args, int);
                    char str[2] = {c, '\0'};
                    VESA_print_unsync(str, COLOR_WHITE);
                    break;
                }
                case 's': {
                    char* s = va_arg(args, char*);
                    VESA_print_unsync(s, COLOR_WHITE);
                    break;
                }
                case 'd': {
                    int d = va_arg(args, int);
                    char buf[32];
                    itoa(d, buf, 10);
                    VESA_print_unsync(buf, COLOR_WHITE);
                    break;
                }
                case 'x': {
                    int x = va_arg(args, int);
                    char buf[32];
                    itoa(x, buf, 16);
                    VESA_print_unsync(buf, COLOR_WHITE);
                    break;
                }
                default:
                    VESA_print_unsync("%", COLOR_WHITE);
                    break;
            }
        } else {
            char str[2] = {format[i], '\0'};
            VESA_print_unsync(str, COLOR_WHITE);
        }
    }
    va_end(args);
}



void kprintf(const char* format, ...) {
    va_list args;
    va_start(args, format);

    for (int i = 0; format[i] != '\0'; i++) {
        if (format[i] == '%') {
            i++;
            switch (format[i]) {
                case 'c': {
                    char c = (char)va_arg(args, int);
                    char str[2] = {c, '\0'};
                    VESA_print(str, COLOR_WHITE);
                    break;
                }
                case 's': {
                    char* s = va_arg(args, char*);
                    VESA_print(s, COLOR_WHITE);
                    break;
                }
                case 'd': {
                    int d = va_arg(args, int);
                    char buf[32];
                    itoa(d, buf, 10);
                    VESA_print(buf, COLOR_WHITE);
                    break;
                }
                case 'x': {
                    int x = va_arg(args, int);
                    char buf[32];
                    itoa(x, buf, 16);
                    VESA_print(buf, COLOR_WHITE);
                    break;
                }
                default:
                    VESA_print("%", COLOR_WHITE);
                    break;
            }
        } else {
            char str[2] = {format[i], '\0'};
            VESA_print(str, COLOR_WHITE);
        }
    }
    va_end(args);
}

void kprintf_color(int color, const char* format, ...) {
    va_list args;
    va_start(args, format);

    for (int i = 0; format[i] != '\0'; i++) {
        if (format[i] == '%') {
            i++;
            switch (format[i]) {
                case 's': {
                    char* s = va_arg(args, char*);
                    VESA_print(s, color); // Use the passed color!
                    break;
                }
                case 'd': {
                    int d = va_arg(args, int);
                    char buf[32];
                    itoa(d, buf, 10);
                    VESA_print(buf, color);
                    break;
                }
                case 'x': {
                    int x = va_arg(args, int);
                    char buf[32];
                    itoa(x, buf, 16);
                    VESA_print("0x", color);
                    VESA_print(buf, color);
                    break;
                }
                case 'c': {
                    char c = (char)va_arg(args, int);
                    char str[2] = {c, '\0'};
                    VESA_print(str, color);
                    break;
                }
            }
        } else {
            char str[2] = {format[i], '\0'};
            VESA_print(str, color);
        }
    }
    va_end(args);
}

// Integer to ASCII
void itoa(int n, char* str, int base) {
    int i = 0;
    int isNegative = 0;

    if (n == 0) {
        str[i++] = '0';
        str[i] = '\0';
        return;
    }

    if (n < 0 && base == 10) {
        isNegative = 1;
        n = -n;
    }

    while (n != 0) {
        int rem = n % base;
        str[i++] = (rem > 9) ? (rem - 10) + 'A' : rem + '0';
        n = n / base;
    }

    if (isNegative) str[i++] = '-';

    str[i] = '\0';

    // The digits are currently backward, let's reverse them
    int start = 0;
    int end = i - 1;
    while (start < end) {
        char temp = str[start];
        str[start] = str[end];
        str[end] = temp;
        start++;
        end--;
    }
}
extern uint32_t system_ticks;

void hexdump(void* ptr, int size) {
    unsigned char* data = (unsigned char*)ptr;
    
    for (int i = 0; i < size; i++) {
        // 1. Print Hex
        unsigned char b = data[i];
        if (b < 0x10) kprintf("0");
        kprintf("%x ", b);

        // 2. Trigger Sidebar every 8 bytes
        if ((i + 1) % 8 == 0) {
            kprintf(" | ");
            for (int j = i - 7; j <= i; j++) {
                char c = data[j];
                if (c >= 32 && c <= 126) kprintf("%c", c);
                else kprintf(".");
            }
            kprintf("\n");
        }
    }

    // 3. FIX: Handle the leftovers!
    int leftovers = size % 8;
    if (leftovers != 0) {
        // Pad the hex area with spaces so the sidebar aligns correctly
        for (int i = 0; i < (8 - leftovers); i++) {
            kprintf("   "); // 3 spaces per missing hex byte (0x + space)
        }

        kprintf(" | ");
        // Print the remaining characters
        for (int i = size - leftovers; i < size; i++) {
            char c = data[i];
            if (c >= 32 && c <= 126) kprintf("%c", c);
            else kprintf(".");
        }
        kprintf("\n");
    }
}

void sleep(int ms) {
    uint32_t end = system_ticks + ms; 
    while (system_ticks < end) {
        yield(); // Call it as a C function, not assembly!
    }
}
int kstrcasecmp(const char* s1, const char* s2) {
    while (*s1 && *s2) {
        char c1 = *s1;
        char c2 = *s2;
        if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
        if (c2 >= 'a' && c2 <= 'z') c2 -= 32;
        if (c1 != c2) return c1 - c2;
        s1++; s2++;
    }
    return *s1 - *s2;
}
const char* get_token(const char* line, char* token_out) {
    while (*line == ' ') line++; // Skip leading spaces
    if (*line == '\0' || *line == '\n' || *line == '\r') return NULL;

    while (*line != ' ' && *line != '\0' && *line != '\n' && *line != '\r') {
        *token_out++ = *line++;
    }
    *token_out = '\0';
    return line;
}
uint32_t katoh(const char* s) {
    uint32_t res = 0;
    // Skip "0x" prefix
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;

    while (1) {
        char c = *s++;
        uint32_t val;
        if (c >= '0' && c <= '9') val = c - '0';
        else if (c >= 'a' && c <= 'f') val = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') val = c - 'A' + 10;
        else break; // End of string or invalid hex char

        res = (res << 4) | (val & 0xF);
    }
    return res;
}
char* kstrcat(char* dest, const char* src) {
    // 1. Move the pointer to the end of the destination string
    char* ptr = dest + kstrlen(dest);

    // 2. Copy src to the end of dest
    while (*src != '\0') {
        *ptr++ = *src++;
    }

    // 3. Add the null terminator
    *ptr = '\0';

    return dest;
}
