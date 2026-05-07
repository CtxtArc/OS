#ifndef VESA_H
#define VESA_H

#include <stdint.h>
#include "font.h"
// Format: 0xRRGGBB
#define COLOR_BLACK   0x000000
#define COLOR_WHITE   0xFFFFFF
#define COLOR_GREEN   0x00FF00
#define COLOR_RED     0xFF0000
#define COLOR_YELLOW  0xFFFF00
#define COLOR_CYAN    0x00FFFF
#define COLOR_BLUE    0x0000FF
#define COLOR_MAGENTA 0xFF00FF
#define COLOR_GREY    0x808080

extern int vesa_cursor_x;
extern int vesa_cursor_y;

struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t num;
    uint32_t size;
    uint32_t addr;
    uint32_t shndx;
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;

    // These are the important ones for graphics:
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
}__attribute__((packed));

void VESA_print_at(const char* str, int x, int y, uint32_t color);
void VESA_init(struct multiboot_info* mbi);
void VESA_putpixel(int x, int y, uint32_t color);
void VESA_print(const char* str, uint32_t color);
void VESA_clear();
void VESA_draw_rect(int x, int y, int w, int h, uint32_t color);
void VESA_draw_char(char c, int x, int y, uint32_t color);
void VESA_scroll();
void VESA_draw_pixel(int x, int y, uint32_t color);

void VESA_print_unsync(const char* str, uint32_t color);
void VESA_flip();
void VESA_flip_rows(int y, int h);
void VESA_clear_buffer_only();
struct multiboot_info* VESA_get_boot_info();
void VESA_set_fps(uint32_t fps);
void VESA_print_at(const char* str, int x, int y, uint32_t color);
void VESA_clear_region(int x, int y, int w, int h);
void kputc(char c);

uint32_t* VESA_get_back_buffer();
#endif // !VESA_H
