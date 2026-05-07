#include "io.h"
#include "task.h"
// A simple circular buffer for the keyboard
static char key_buffer[256];
static int head = 0;
static int tail = 0;
extern int keyboard_focus_tid;
extern int current_task_idx;
extern volatile struct task task_list[MAX_TASKS];
// This is what the Linker is looking for!
int has_key_in_buffer() {
    return head != tail;
}

char get_key_from_buffer() {
    if (head == tail) return 0;
    char c = key_buffer[tail];
    tail = (tail + 1) % 256;
    return c;
}

// This is what your IRQ1 handler should call when a key is pressed
void keyboard_push_char(char c) {
    int next = (head + 1) % 256;
    if (next != tail) { // Don't overwrite if buffer is full
        key_buffer[head] = c;
        head = next;
    }
}

uint8_t keyboard_read_status() {
    return inb(0x64);
}

uint8_t keyboard_read_scancode() {
    return inb(0x60);
}


char keyboard_getchar() {
    while (1) {
        // 1. Check focus. If we don't have the microphone, just wait.
        if (current_task_idx != keyboard_focus_tid) {
            yield();
            continue;
        }

        volatile struct task *me = &task_list[current_task_idx];

        // 2. Atomic check of the mailbox
        asm volatile("cli");
        if (me->kbd_head != me->kbd_tail) {
            char c = me->kbd_buffer[me->kbd_tail];
            me->kbd_tail = (me->kbd_tail + 1) % 64; // Make sure this matches your task struct size!
            asm volatile("sti");
            return c;
        }

        // 3. NO KEY: Put the task to sleep (Block)
        me->state = 3; 
        asm volatile("sti");
        
        // 4. Sleep until the keyboard wakes us
        yield(); 
    }
}

char scancode_to_ascii(uint8_t scancode, int shift) {
    switch(scancode) {
        // Number row
        case 0x02: return shift ? '!' : '1';
        case 0x03: return shift ? '@' : '2';
        case 0x04: return shift ? '#' : '3';
        case 0x05: return shift ? '$' : '4';
        case 0x06: return shift ? '%' : '5';
        case 0x07: return shift ? '^' : '6';
        case 0x08: return shift ? '&' : '7';
        case 0x09: return shift ? '*' : '8';
        case 0x0A: return shift ? '(' : '9';
        case 0x0B: return shift ? ')' : '0';
        case 0x0C: return shift ? '_' : '-';
        case 0x0D: return shift ? '+' : '=';

        // Letters (QWERTY)
        case 0x10: return shift ? 'Q' : 'q';
        case 0x11: return shift ? 'W' : 'w';
        case 0x12: return shift ? 'E' : 'e';
        case 0x13: return shift ? 'R' : 'r';
        case 0x14: return shift ? 'T' : 't';
        case 0x15: return shift ? 'Y' : 'y';
        case 0x16: return shift ? 'U' : 'u';
        case 0x17: return shift ? 'I' : 'i';
        case 0x18: return shift ? 'O' : 'o';
        case 0x19: return shift ? 'P' : 'p';
        case 0x1E: return shift ? 'A' : 'a';
        case 0x1F: return shift ? 'S' : 's';
        case 0x20: return shift ? 'D' : 'd';
        case 0x21: return shift ? 'F' : 'f';
        case 0x22: return shift ? 'G' : 'g';
        case 0x23: return shift ? 'H' : 'h';
        case 0x24: return shift ? 'J' : 'j';
        case 0x25: return shift ? 'K' : 'k';
        case 0x26: return shift ? 'L' : 'l';
        case 0x2C: return shift ? 'Z' : 'z';
        case 0x2D: return shift ? 'X' : 'x';
        case 0x2E: return shift ? 'C' : 'c';
        case 0x2F: return shift ? 'V' : 'v';
        case 0x30: return shift ? 'B' : 'b';
        case 0x31: return shift ? 'N' : 'n';
        case 0x32: return shift ? 'M' : 'm';

        // Punctuation
        case 0x1A: return shift ? '{' : '[';
        case 0x1B: return shift ? '}' : ']';
        case 0x27: return shift ? ':' : ';';
        case 0x28: return shift ? '"' : '\'';
        case 0x29: return shift ? '~' : '`';
        case 0x2B: return shift ? '|' : '\\';
        case 0x33: return shift ? '<' : ',';
        case 0x34: return shift ? '>' : '.';
        case 0x35: return shift ? '?' : '/';

        // Functional
        case 0x1C: return '\n'; // Enter
        case 0x39: return ' ';  // Space
        case 0x0E: return '\b'; // Backspace
        default:   return 0;
    }
}
// Read 16 bits from the specified I/O port
uint16_t inw(uint16_t port) {
    uint16_t result;
    __asm__ volatile ("inw %1, %0" : "=a" (result) : "dN" (port));
    return result;
}

// Write 16 bits to the specified I/O port
void outw(uint16_t port, uint16_t data) {
    __asm__ volatile ("outw %0, %1" : : "a" (data), "dN" (port));
}
