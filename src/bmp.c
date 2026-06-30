#include "bmp.h"
#include "fat.h"
#include "kheap.h"
#include "lib.h"
#include "vesa.h"

// We store the parsed image permanently in RAM so the
// Compositor doesn't lag while redrawing it every frame!
extern uint32_t *desktop_bg_buffer;

void load_desktop_wallpaper(const char *filename) {
  struct fat_dir_entry *entry = fat_search(filename);
  if (!entry) {
    kprintf("Wallpaper '%s' not found on FAT disk.\n", filename);
    return;
  }

  uint8_t *raw_data = (uint8_t *)fat_load_file(entry);
  kfree(entry); // fat_search() heap-allocates; we only needed it for
                // fat_load_file.

  if (!raw_data)
    return;

  struct bmp_file_header *file_hdr = (struct bmp_file_header *)raw_data;
  struct bmp_info_header *info_hdr =
      (struct bmp_info_header *)(raw_data + sizeof(struct bmp_file_header));

  // Check if it's actually a BMP
  if (file_hdr->file_type != 0x4D42) { // 'BM'
    kprintf("Error: Not a valid BMP file.\n");
    kfree(raw_data);
    return;
  }
  if (info_hdr->bit_count != 24) {
    kprintf("Error: Only 24-bit uncompressed BMPs are supported.\n");
    kfree(raw_data);
    return;
  }

  struct multiboot_info *mbi = VESA_get_boot_info();
  uint32_t sw = mbi->framebuffer_width;
  uint32_t sh = mbi->framebuffer_height;

  // Point to the actual pixel data using the offset
  uint8_t *pixel_data = raw_data + file_hdr->offset_data;

  // BMP rows are padded to a multiple of 4 bytes
  int row_padding = (4 - (info_hdr->width * 3) % 4) % 4;

  // Read the BMP and stretch/crop it into the permanent Desktop Buffer
  for (int y = 0; y < info_hdr->height && y < (int)sh; y++) {

    // BMPs are stored upside down! Calculate the inverted Y.
    int draw_y = info_hdr->height - 1 - y;
    if (draw_y >= (int)sh)
      draw_y = sh - 1; // Crop if image is taller than screen

    for (int x = 0; x < info_hdr->width && x < (int)sw; x++) {

      // Calculate where we are in the raw file array
      int pixel_index = (y * (info_hdr->width * 3 + row_padding)) + (x * 3);

      // Extract BGR and convert to 0xRRGGBB format
      uint8_t b = pixel_data[pixel_index + 0];
      uint8_t g = pixel_data[pixel_index + 1];
      uint8_t r = pixel_data[pixel_index + 2];
      uint32_t color = (r << 16) | (g << 8) | b;

      // Draw to our permanent background buffer
      desktop_bg_buffer[draw_y * sw + x] = color;
    }
  }
  kfree(raw_data);
}
