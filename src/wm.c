#include "wm.h"
#include "kheap.h"
#include "lib.h"
#include "task.h"
#include "vesa.h"

volatile uint32_t fps_counter = 0;
volatile uint32_t current_fps = 0;
uint32_t last_fps_tick = 0;

extern uint32_t system_ticks;

extern uint32_t *desktop_bg_buffer;
extern int pending_shell_spawn;
extern volatile int keyboard_focus_tid;
extern int vesa_updating;
extern int vesa_dirty;
extern void shell_task(); // So the WM can spawn the shell

void compositor_task() {
  struct multiboot_info *mbi = VESA_get_boot_info();
  uint32_t sw = mbi->framebuffer_width;
  uint32_t sh = mbi->framebuffer_height;
  int last_gui_count = -1;

  while (1) {
    if (vesa_updating) {
      yield();
      continue;
    }
    uint32_t *b_buffer = VESA_get_back_buffer();
    if (pending_shell_spawn) {
      pending_shell_spawn = 0;
      int new_tid = spawn_task(shell_task, NULL, "shell");
      if (new_tid != -1) {
        task_create_window(new_tid, 0, 0, 0, 0);
        keyboard_focus_tid = new_tid;
      }
    }

    int gui_tasks = 0;
    int global_needs_update = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
      if (task_list[i].state != 0 && task_list[i].has_window &&
          task_list[i].window_ready) {
        gui_tasks++;
        if (task_list[i].is_dirty)
          global_needs_update = 1;
      }
    }

    if (gui_tasks != last_gui_count) {
      last_gui_count = gui_tasks;
      refresh_tiling_layout();

      // THE FIX: Temporary local pointers so Assembly doesn't destroy our
      // variables!
      if (desktop_bg_buffer) {
        uint32_t count = sw * sh;
        uint32_t *tmp_dest = b_buffer;
        uint32_t *tmp_src = desktop_bg_buffer;
        __asm__ volatile("rep movsl"
                         : "+D"(tmp_dest), "+S"(tmp_src), "+c"(count)
                         :
                         : "memory");
      } else {
        uint32_t count = sw * sh;
        uint32_t val = 0x000033;
        uint32_t *tmp_dest = b_buffer;
        __asm__ volatile("rep stosl"
                         : "+D"(tmp_dest), "+c"(count)
                         : "a"(val)
                         : "memory");
      }

      int tile_width = (gui_tasks > 0) ? sw / gui_tasks : sw;
      int current_tile = 0;

      for (int i = 0; i < MAX_TASKS; i++) {
        if (task_list[i].state != 0 && task_list[i].has_window &&
            task_list[i].window_ready && task_list[i].window_buffer) {

          int start_x = current_tile * tile_width;
          int current_tile_w = (current_tile == gui_tasks - 1)
                                   ? (int)(sw - start_x)
                                   : tile_width;

          for (uint32_t y = 0; y < sh; y++) {
            uint32_t *tmp_dest = &b_buffer[y * sw + start_x];
            uint32_t *tmp_src = &task_list[i].window_buffer[y * sw];
            uint32_t count = current_tile_w;
            __asm__ volatile("rep movsl"
                             : "+D"(tmp_dest), "+S"(tmp_src), "+c"(count)
                             :
                             : "memory");
          }

          uint32_t border_color =
              (keyboard_focus_tid == i) ? 0x00FFFF : 0x555555;
          for (int b = 0; b < WIN_BORDER; b++) {
            for (int x = 0; x < current_tile_w; x++) {
              b_buffer[(b * sw) + start_x + x] = border_color;
              b_buffer[((sh - 1 - b) * sw) + start_x + x] = border_color;
            }
            for (uint32_t y = 0; y < sh; y++) {
              b_buffer[(y * sw) + start_x + b] = border_color;
              b_buffer[(y * sw) + start_x + current_tile_w - 1 - b] =
                  border_color;
            }
          }

          current_tile++;
        }
      }
      vesa_dirty = 1;
      VESA_flip();
      continue;
    }

    if (global_needs_update) {
      int tile_width = (gui_tasks > 0) ? sw / gui_tasks : sw;
      int current_tile = 0;
      int min_x = sw, min_y = sh, max_x = 0, max_y = 0;
      int did_draw = 0;

      for (int i = 0; i < MAX_TASKS; i++) {
        if (task_list[i].state != 0 && task_list[i].has_window &&
            task_list[i].window_ready && task_list[i].window_buffer) {

          if (task_list[i].is_dirty) {
            int dx = task_list[i].dirty_x, dy = task_list[i].dirty_y;
            int dw = task_list[i].dirty_w, dh = task_list[i].dirty_h;
            task_list[i].is_dirty = 0;
            int start_x = current_tile * tile_width;
            int current_tile_w = (current_tile == gui_tasks - 1)
                                     ? (int)(sw - start_x)
                                     : tile_width;

            if (dx + dw > current_tile_w)
              dw = current_tile_w - dx;
            if (dy + dh > (int)sh)
              dh = sh - dy;

            if (dw > 0 && dh > 0) {
              for (int r = 0; r < dh; r++) {
                uint32_t *tmp_dest =
                    &b_buffer[((dy + r) * sw) + (start_x + dx)];
                uint32_t *tmp_src =
                    &task_list[i].window_buffer[((dy + r) * sw) + dx];
                uint32_t count = dw;
                __asm__ volatile("rep movsl"
                                 : "+D"(tmp_dest), "+S"(tmp_src), "+c"(count)
                                 :
                                 : "memory");
              }

              uint32_t border_color =
                  (keyboard_focus_tid == i) ? 0x00FFFF : 0x555555;
              for (int b = 0; b < WIN_BORDER; b++) {
                for (int x = 0; x < current_tile_w; x++) {
                  b_buffer[(b * sw) + start_x + x] = border_color;
                  b_buffer[((sh - 1 - b) * sw) + start_x + x] = border_color;
                }
                for (uint32_t y = 0; y < sh; y++) {
                  b_buffer[(y * sw) + start_x + b] = border_color;
                  b_buffer[(y * sw) + start_x + current_tile_w - 1 - b] =
                      border_color;
                }
              }

              if (start_x + dx < min_x)
                min_x = start_x + dx;
              if (dy < min_y)
                min_y = dy;
              if (start_x + dx + dw > max_x)
                max_x = start_x + dx + dw;
              if (dy + dh > max_y)
                max_y = dy + dh;
              did_draw = 1;
            }
          }
          current_tile++;
        }
      }
      if (did_draw) {
        VESA_update_rect(min_x, min_y, max_x - min_x, max_y - min_y);
        fps_counter++;

        if (system_ticks - last_fps_tick >= 1000) {
          current_fps = fps_counter;
          fps_counter = 0;
          last_fps_tick = system_ticks;
        }
      }
    }
    yield();
  }
}
void task_create_window(int tid, int x, int y, int w, int h) {
  if (tid < 0 || tid >= MAX_TASKS)
    return;
  struct multiboot_info *mbi = VESA_get_boot_info();
  uint32_t sw = mbi->framebuffer_width;
  uint32_t sh = mbi->framebuffer_height;

  // LOCK: Ensure the compositor doesn't touch this yet
  task_list[tid].window_ready = 0;
  task_list[tid].has_window = 0;

  if (task_list[tid].window_buffer == NULL) {
    task_list[tid].window_buffer = (uint32_t *)kmalloc(sw * sh * 4);
  }

  if (task_list[tid].window_buffer) {
    // Fast fill background
    uint32_t count = sw * sh;
    uint32_t val = 0x222222;
    uint32_t *dest = task_list[tid].window_buffer;
    __asm__ volatile("rep stosl"
                     : "+D"(dest), "+c"(count)
                     : "a"(val)
                     : "memory");

    task_list[tid].win_x = x;
    task_list[tid].win_y = y;
    task_list[tid].win_w = (w == 0) ? (int)sw : w;
    task_list[tid].win_h = (h == 0) ? (int)sh : h;
    task_list[tid].cursor_x = 0;
    task_list[tid].cursor_y = 0;

    // FORCE tiling refresh to assign actual screen coordinates
    refresh_tiling_layout();

    // UNLOCK: compositor can now see this window
    task_list[tid].has_window = 1;
    task_list[tid].window_ready = 1;

    // Force a GLOBAL redraw to make the new tile appear instantly
    for (int i = 0; i < MAX_TASKS; i++) {
      if (task_list[i].state != 0)
        mark_task_dirty(i, 0, 0, 4000, 4000);
    }
  }
}
void refresh_tiling_layout() {
  struct multiboot_info *mbi = VESA_get_boot_info();
  uint32_t sw = mbi->framebuffer_width;
  uint32_t sh = mbi->framebuffer_height;

  int gui_tasks = 0;
  for (int i = 0; i < MAX_TASKS; i++) {
    if (task_list[i].state != 0 && task_list[i].has_window)
      gui_tasks++;
  }

  if (gui_tasks > 0) {
    int tile_width = sw / gui_tasks;
    int border = WIN_BORDER;
    int current_tile = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
      if (task_list[i].state != 0 && task_list[i].has_window) {
        int start_x = current_tile * tile_width;
        task_list[i].win_x = start_x;

        if (current_tile == gui_tasks - 1) {
          task_list[i].win_w = (sw - start_x) - (border * 2);
        } else {
          task_list[i].win_w = tile_width - (border * 2);
        }

        task_list[i].win_h = sh - (border * 2);
        current_tile++;
      }
    }
  }
}
// --- DIRTY RECTANGLE HELPER ---
void mark_task_dirty(int id, int x, int y, int w, int h) {
  volatile struct task *t = &task_list[id];
  if (!t->is_dirty) {
    t->dirty_x = x;
    t->dirty_y = y;
    t->dirty_w = w;
    t->dirty_h = h;
    t->is_dirty = 1;
  } else {
    int right =
        (x + w > t->dirty_x + t->dirty_w) ? (x + w) : (t->dirty_x + t->dirty_w);
    int bottom =
        (y + h > t->dirty_y + t->dirty_h) ? (y + h) : (t->dirty_y + t->dirty_h);
    if (x < t->dirty_x)
      t->dirty_x = x;
    if (y < t->dirty_y)
      t->dirty_y = y;
    t->dirty_w = right - t->dirty_x;
    t->dirty_h = bottom - t->dirty_y;
  }
}
