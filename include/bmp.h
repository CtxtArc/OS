#ifndef BMP_H
#define BMP_H

#include <stdint.h>

// 14 bytes
struct __attribute__((packed)) bmp_file_header {
    uint16_t file_type;     // Always 'BM' (0x4D42)
    uint32_t file_size;
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t offset_data;   // Where the pixel array starts
};

// 40 bytes
struct __attribute__((packed)) bmp_info_header {
    uint32_t size;
    int32_t  width;
    int32_t  height;
    uint16_t planes;
    uint16_t bit_count;     // We only want 24-bit (3 bytes per pixel)
    uint32_t compression;   // We only want 0 (uncompressed)
    uint32_t size_image;
    int32_t  x_pixels_per_meter;
    int32_t  y_pixels_per_meter;
    uint32_t colors_used;
    uint32_t colors_important;
};

void load_desktop_wallpaper(const char* filename);

#endif
