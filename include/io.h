#ifndef IO_H
#define IO_H

#include <stdint.h>

#define KEYBOARD_BUFFER_SIZE 256

// Shared variables
extern char kbd_buffer[KEYBOARD_BUFFER_SIZE];
extern int kbd_head;
extern int kbd_tail;

int has_key_in_buffer();
char get_key_from_buffer();
void keyboard_push_char(char c);
// Function to pull a char from the buffer
char keyboard_getchar();
// Must be in the header so every file can "paste" this assembly code
__attribute__((always_inline)) static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ( "inb %w1, %b0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

__attribute__((always_inline)) static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ( "outb %b0, %w1" : : "a"(val), "Nd"(port) : "memory");
}

/* --- Function Prototypes (Logic in io.c) --- */
uint8_t keyboard_read_status();
uint8_t keyboard_read_scancode();
char scancode_to_ascii(uint8_t scancode, int shift, int altgr);
uint16_t inw(uint16_t port);
void outw(uint16_t port, uint16_t data);
#endif
