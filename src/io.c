#include "io.h"
#include "task.h"

// --- CONFIGURATION ---
#define KEYBOARD_LAYOUT_AZERTY
// #define KEYBOARD_LAYOUT_QWERTY
// ---------------------

extern int keyboard_focus_tid;
extern int current_task_idx;
extern volatile struct task task_list[MAX_TASKS];

// State trackers
extern int ctrl_state;
extern int shift_state;
extern int alt_state;
extern int altgr_state ; // Tracks AltGR (Right Alt)
extern int is_extended;

uint16_t inw(uint16_t port) {
    uint16_t result;
    __asm__ volatile ("inw %1, %0" : "=a" (result) : "dN" (port));
    return result;
}

void outw(uint16_t port, uint16_t data) {
    __asm__ volatile ("outw %0, %1" : : "a" (data), "dN" (port));
}

char scancode_to_ascii(uint8_t scancode, int shift, int altgr) {
    switch(scancode) {
        // --- Row 1 (Numbers / Symbols) ---
        case 0x02: // 1
            #ifdef KEYBOARD_LAYOUT_AZERTY
                if (altgr) return 0; // AZERTY has nothing here usually
                return shift ? '1' : '&';
            #else
                return shift ? '!' : '1';
            #endif
        case 0x03: // 2
            #ifdef KEYBOARD_LAYOUT_AZERTY
                if (altgr) return '~';
                return shift ? '2' : 'e'; // Simplified 'é'
            #else
                if (altgr) return 0;
                return shift ? '@' : '2';
            #endif
        case 0x04: // 3
            #ifdef KEYBOARD_LAYOUT_AZERTY
                if (altgr) return '#';
                return shift ? '3' : '"';
            #else
                return shift ? '#' : '3';
            #endif
        case 0x05: // 4
            #ifdef KEYBOARD_LAYOUT_AZERTY
                if (altgr) return '{';
                return shift ? '4' : '\'';
            #else
                return shift ? '$' : '4';
            #endif
        case 0x06: // 5
            #ifdef KEYBOARD_LAYOUT_AZERTY
                if (altgr) return '[';
                return shift ? '5' : '(';
            #else
                return shift ? '%' : '5';
            #endif
        case 0x07: // 6
            #ifdef KEYBOARD_LAYOUT_AZERTY
                if (altgr) return '|';
                return shift ? '6' : '-';
            #else
                return shift ? '^' : '6';
            #endif
        case 0x08: // 7
            #ifdef KEYBOARD_LAYOUT_AZERTY
                if (altgr) return '`';
                return shift ? '7' : 'e'; // Simplified 'è'
            #else
                return shift ? '&' : '7';
            #endif
        case 0x09: // 8
            #ifdef KEYBOARD_LAYOUT_AZERTY
                if (altgr) return '\\';
                return shift ? '8' : '_';
            #else
                return shift ? '*' : '8';
            #endif
        case 0x0A: // 9
            #ifdef KEYBOARD_LAYOUT_AZERTY
                if (altgr) return '^';
                return shift ? '9' : 'c'; // Simplified 'ç'
            #else
                return shift ? '(' : '9';
            #endif
        case 0x0B: // 0
            #ifdef KEYBOARD_LAYOUT_AZERTY
                if (altgr) return '@';
                return shift ? '0' : 'a'; // Simplified 'à'
            #else
                return shift ? ')' : '0';
            #endif
        case 0x0C: // - ) ]
            #ifdef KEYBOARD_LAYOUT_AZERTY
                if (altgr) return ']';
                return shift ? 0 : ')';
            #else
                return shift ? '_' : '-';
            #endif
        case 0x0D: // = + }
            #ifdef KEYBOARD_LAYOUT_AZERTY
                if (altgr) return '}';
                return shift ? '+' : '=';
            #else
                return shift ? '+' : '=';
            #endif

        // --- Alphabetical Keys ---
        case 0x10: 
            #ifdef KEYBOARD_LAYOUT_AZERTY
                return shift ? 'A' : 'a';
            #else
                return shift ? 'Q' : 'q';
            #endif
        case 0x11: 
            #ifdef KEYBOARD_LAYOUT_AZERTY
                return shift ? 'Z' : 'z';
            #else
                return shift ? 'W' : 'w';
            #endif
        case 0x12: return shift ? 'E' : 'e';
        case 0x13: return shift ? 'R' : 'r';
        case 0x14: return shift ? 'T' : 't';
        case 0x15: return shift ? 'Y' : 'y';
        case 0x16: return shift ? 'U' : 'u';
        case 0x17: return shift ? 'I' : 'i';
        case 0x18: return shift ? 'O' : 'o';
        case 0x19: return shift ? 'P' : 'p';
        case 0x1E: 
            #ifdef KEYBOARD_LAYOUT_AZERTY
                return shift ? 'Q' : 'q';
            #else
                return shift ? 'A' : 'a';
            #endif
        case 0x1F: return shift ? 'S' : 's';
        case 0x20: return shift ? 'D' : 'd';
        case 0x21: return shift ? 'F' : 'f';
        case 0x22: return shift ? 'G' : 'g';
        case 0x23: return shift ? 'H' : 'h';
        case 0x24: return shift ? 'J' : 'j';
        case 0x25: return shift ? 'K' : 'k';
        case 0x26: return shift ? 'L' : 'l';
        case 0x27: 
            #ifdef KEYBOARD_LAYOUT_AZERTY
                return shift ? 'M' : 'm';
            #else
                return shift ? ':' : ';';
            #endif
        case 0x2C: 
            #ifdef KEYBOARD_LAYOUT_AZERTY
                return shift ? 'W' : 'w';
            #else
                return shift ? 'Z' : 'z';
            #endif
        case 0x2D: return shift ? 'X' : 'x';
        case 0x2E: return shift ? 'C' : 'c';
        case 0x2F: return shift ? 'V' : 'v';
        case 0x30: return shift ? 'B' : 'b';
        case 0x31: return shift ? 'N' : 'n';
        case 0x32: 
            #ifdef KEYBOARD_LAYOUT_AZERTY
                return shift ? '?' : ',';
            #else
                return shift ? 'M' : 'm';
            #endif
        case 0x33: 
            #ifdef KEYBOARD_LAYOUT_AZERTY
                return shift ? '.' : ';';
            #else
                return shift ? '<' : ',';
            #endif
        case 0x34: 
            #ifdef KEYBOARD_LAYOUT_AZERTY
                return shift ? '/' : ':';
            #else
                return shift ? '>' : '.';
            #endif
        case 0x35: 
            #ifdef KEYBOARD_LAYOUT_AZERTY
                return shift ? '!' : '!'; // Simplified
            #else
                return shift ? '?' : '/';
            #endif

        case 0x1C: return '\n';
        case 0x39: return ' '; 
        case 0x0E: return '\b';
        case 0x0F: return '\t';
        case 0x56: 
            #ifdef KEYBOARD_LAYOUT_AZERTY
                return shift ? '>' : '<';
            #else
                // On QWERTY, this scancode usually doesn't exist 
                // or maps to \ | depending on the region
                return shift ? '>' : '<'; 
            #endif
        default: return 0;
    }
}
int has_key_in_buffer() {
    volatile struct task* me = &task_list[current_task_idx];
    if (me->kbd_head >= 64) me->kbd_head = 0;
    if (me->kbd_tail >= 64) me->kbd_tail = 0;
    return me->kbd_head != me->kbd_tail;
}

char get_key_from_buffer() {
    volatile struct task* me = &task_list[current_task_idx];
    if (me->kbd_head >= 64) me->kbd_head = 0;
    if (me->kbd_tail >= 64) me->kbd_tail = 0;
    if (me->kbd_head == me->kbd_tail) return 0;
    
    char c = me->kbd_buffer[me->kbd_tail];
    me->kbd_tail = (me->kbd_tail + 1) % 64;
    return c;
}

char keyboard_getchar() {
    volatile struct task *me = &task_list[current_task_idx];

    while (1) {
        // --- THE FIX: Block immediately if we don't have focus ---
        if (current_task_idx != keyboard_focus_tid) {
            __asm__ volatile("cli");
            me->state = 3; // Block (0% CPU)
            __asm__ volatile("sti");
            yield();
            continue;
        }

        // --- We HAVE focus. Check the buffer safely ---
        __asm__ volatile("cli");
        if (me->kbd_head >= 64) me->kbd_head = 0;
        if (me->kbd_tail >= 64) me->kbd_tail = 0;

        if (me->kbd_head != me->kbd_tail) {
            char c = me->kbd_buffer[me->kbd_tail];
            me->kbd_tail = (me->kbd_tail + 1) % 64; 
            __asm__ volatile("sti");
            return c;
        }

        // --- Buffer is empty. Block until next keypress ---
        me->state = 3; // Block (0% CPU)
        __asm__ volatile("sti");
        yield(); 
    }
}
