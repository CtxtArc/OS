#include "vesa.h"
#include "io.h"
#include "kheap.h"
#include "lib.h"
#include "task.h"

extern volatile struct task task_list[MAX_TASKS];
static struct multiboot_info *boot_info = 0;
int vesa_dirty = 0;
int vesa_updating = 0;
extern int current_task_idx;
static uint32_t *back_buffer = NULL;
static uint32_t total_pixels = 0;
static uint32_t screen_width = 0;

uint32_t target_fps = 30;

// Link the dirty tracker from idt.c
extern void mark_task_dirty(int id, int x, int y, int w, int h);

struct multiboot_info *VESA_get_boot_info() { return boot_info; }
uint32_t *VESA_get_back_buffer() { return back_buffer; }

// Waits for the monitor to finish drawing the current frame (QEMU SAFE)
void VESA_wait_vsync() {
  volatile uint32_t timeout = 100000;

  // 1. Wait for previous retrace to end
  while ((inb(0x3DA) & 8) && --timeout)
    ;

  timeout = 100000;

  // 2. Wait for new retrace to begin
  while (!(inb(0x3DA) & 8) && --timeout)
    ;
}

void VESA_init(struct multiboot_info *mbi) {
  boot_info = mbi;
  screen_width = boot_info->framebuffer_width;
  total_pixels = screen_width * boot_info->framebuffer_height;
  back_buffer = (uint32_t *)kmalloc(total_pixels * 4);
  VESA_clear();
}

void VESA_draw_rect(int x, int y, int w, int h, uint32_t color) {
  volatile struct task *cur = &task_list[current_task_idx];
  uint32_t sw = screen_width;

  if (cur->has_window && cur->window_buffer != NULL) {
    for (int iy = 0; iy < h; iy++) {
      for (int ix = 0; ix < w; ix++) {
        int dx = x + ix;
        int dy = y + iy;
        if (dx >= 0 && dx < cur->win_w && dy >= 0 && dy < cur->win_h) {
          cur->window_buffer[dy * sw + dx] = color;
        }
      }
    }
    // AUTO-TRACK DIRT
    mark_task_dirty(current_task_idx, x, y, w, h);
  } else {
    if (!back_buffer)
      return;
    for (int i = 0; i < h; i++) {
      uint32_t *row = back_buffer + ((y + i) * sw) + x;
      for (int j = 0; j < w; j++)
        row[j] = color;
    }
    vesa_dirty = 1;
  }
}

void VESA_draw_char(char c, int x, int y, uint32_t color) {
  volatile struct task *cur = &task_list[current_task_idx];
  uint8_t *glyph = font8x8_basic[(int)c];
  uint32_t sw = screen_width;
  uint32_t bg = 0x222222;

  for (int row = 0; row < 8; row++) {
    uint8_t data = glyph[row];
    for (int col = 0; col < 8; col++) {
      uint32_t pix = (data & (1 << (7 - col))) ? color : (c == ' ' ? bg : 0);
      if (pix == 0 && c != ' ')
        continue;

      int dx = x + col;
      int dy = y + row;

      if (cur->has_window && cur->window_buffer != NULL) {
        if (dx >= 0 && dx < cur->win_w && dy >= 0 && dy < cur->win_h) {
          cur->window_buffer[dy * sw + dx] = pix;
        }
      } else if (back_buffer) {
        if (dx >= 0 && dx < (int)sw && dy >= 0 &&
            dy < (int)boot_info->framebuffer_height) {
          back_buffer[dy * sw + dx] = pix;
          vesa_dirty = 1;
        }
      }
    }
  }

  if (cur->has_window) {
    // AUTO-TRACK DIRT (8x8 character block)
    mark_task_dirty(current_task_idx, x, y, 8, 8);
  }
}

void VESA_print(const char *str, uint32_t color) {
  volatile struct task *cur = &task_list[current_task_idx];
  int max_w = cur->has_window ? cur->win_w : (int)screen_width;
  int max_h = cur->has_window ? cur->win_h : (int)boot_info->framebuffer_height;

  while (*str) {
    if (cur->cursor_y > max_h - 20)
      VESA_scroll();

    if (*str == '\n') {
      cur->cursor_x = 0;
      cur->cursor_y += 10;
    } else {
      if (cur->cursor_x + 8 >= max_w) {
        cur->cursor_x = 0;
        cur->cursor_y += 10;
      }
      VESA_draw_char(*str, cur->cursor_x, cur->cursor_y, color);
      cur->cursor_x += 8;
    }
    str++;
  }
}

void VESA_scroll() {
  volatile struct task *cur = &task_list[current_task_idx];
  uint32_t sw = screen_width;
  uint32_t line = 10;

  if (cur->has_window && cur->window_buffer != NULL) {
    uint32_t h = cur->win_h;
    kmemcpy32(cur->window_buffer, cur->window_buffer + (line * sw),
              (h - line) * sw);
    uint32_t *bottom = cur->window_buffer + (h - line) * sw;
    for (uint32_t i = 0; i < line * sw; i++)
      bottom[i] = 0x222222;

    // Scrolling changes the entire window!
    mark_task_dirty(current_task_idx, 0, 0, cur->win_w, cur->win_h);
  } else if (back_buffer) {
    uint32_t h = boot_info->framebuffer_height;
    kmemcpy32(back_buffer, back_buffer + (line * sw), (h - line) * sw);
    uint32_t *bottom = back_buffer + (h - line) * sw;
    for (uint32_t i = 0; i < line * sw; i++)
      bottom[i] = 0x000033;
    vesa_dirty = 1;
  }
  cur->cursor_y -= line;
}

void VESA_clear_buffer_only() {
  volatile struct task *cur = &task_list[current_task_idx];
  uint32_t sw = screen_width;

  if (cur->has_window && cur->window_buffer != NULL) {
    for (uint32_t row = 0; row < (uint32_t)cur->win_h; row++) {
      for (uint32_t col = 0; col < sw; col++) {
        cur->window_buffer[row * sw + col] = 0x222222;
      }
    }
    cur->cursor_x = 0;
    cur->cursor_y = 0;
    mark_task_dirty(current_task_idx, 0, 0, cur->win_w, cur->win_h);
  } else if (back_buffer) {
    for (uint32_t p = 0; p < total_pixels; p++)
      back_buffer[p] = 0x000033;
    vesa_dirty = 1;
  }
}

void VESA_flip() {
  if (!back_buffer || !vesa_dirty || vesa_updating)
    return;
  uint32_t *vram = (uint32_t *)(uintptr_t)boot_info->framebuffer_addr;

  VESA_wait_vsync(); // <--- THE FIX
  kmemcpy32(vram, back_buffer, total_pixels);

  vesa_dirty = 0;
}

void VESA_update_rect(int x, int y, int w, int h) {
  if (!back_buffer || vesa_updating)
    return;

  uint32_t sw = screen_width;
  uint32_t sh = boot_info->framebuffer_height;
  uint32_t *vram = (uint32_t *)(uintptr_t)boot_info->framebuffer_addr;

  if (x < 0)
    x = 0;
  if (y < 0)
    y = 0;
  if (x + w > (int)sw)
    w = sw - x;
  if (y + h > (int)sh)
    h = sh - y;
  if (w <= 0 || h <= 0)
    return;

  VESA_wait_vsync(); // <--- THE FIX

  for (int row = 0; row < h; row++) {
    uint32_t offset = ((y + row) * sw) + x;
    kmemcpy32(&vram[offset], &back_buffer[offset], w);
  }
}
void VESA_clear() {
  VESA_clear_buffer_only();
  VESA_flip();
}

void kputc(char c) {
  if (!boot_info)
    return;
  volatile struct task *cur = &task_list[current_task_idx];

  if (c == '\n') {
    cur->cursor_x = 0;
    cur->cursor_y += 10;
    return;
  }
  if (c == '\r') {
    cur->cursor_x = 0;
    return;
  }

  int max_w = cur->has_window ? cur->win_w : (int)screen_width;
  if (cur->cursor_x + 8 >= max_w) {
    cur->cursor_x = 0;
    cur->cursor_y += 10;
  }

  int max_h = cur->has_window ? cur->win_h : (int)boot_info->framebuffer_height;
  if (cur->cursor_y > max_h - 20)
    VESA_scroll();

  VESA_draw_char(c, cur->cursor_x, cur->cursor_y, 0xFFFFFF);
  cur->cursor_x += 8;
}

void VESA_print_at(const char *str, int x, int y, uint32_t color) {
  if (!back_buffer)
    return;
  while (*str) {
    VESA_draw_char(*str, x, y, color);
    x += 8;
    str++;
  }
}

void VESA_print_unsync(const char *str, uint32_t color) {
  if (!str)
    return;
  volatile struct task *cur = &task_list[current_task_idx];

  while (*str) {
    int max_h =
        cur->has_window ? cur->win_h : (int)boot_info->framebuffer_height;
    if (cur->cursor_y > max_h - 20)
      VESA_scroll();

    if (*str == '\n') {
      cur->cursor_x = 0;
      cur->cursor_y += 10;
    } else {
      int max_w = cur->has_window ? cur->win_w : (int)screen_width;
      if (cur->cursor_x + 8 >= max_w) {
        cur->cursor_x = 0;
        cur->cursor_y += 10;
      }
      VESA_draw_char(*str, cur->cursor_x, cur->cursor_y, color);
      cur->cursor_x += 8;
    }
    str++;
  }
}

void VESA_clear_region(int x, int y, int w, int h) {
  VESA_draw_rect(x, y, w, h, 0x000033);
}
