#include "fat.h"
#include "kheap.h"
#include "lib.h"
#include "task.h"
#include "vesa.h"
#include "vfs.h"

extern volatile int keyboard_focus_tid;
extern uint32_t system_ticks;
extern volatile int current_task_idx;
extern volatile struct task task_list[];

extern void mark_task_dirty(int id, int x, int y, int w, int h);

void KED_task() {
  __asm__ volatile("mov $8, %eax; int $0x80");
  // 1. LOCAL STATE INITIALIZATION
  // Capture global filename into local stack to prevent ghosting/concurrency
  // issues
  char local_filename[16];
  // Grab the filename the Shell safely passed to us via code_ptr
  char *passed_filename = (char *)task_list[current_task_idx].code_ptr;

  if (passed_filename) {
    kstrncpy(local_filename, passed_filename, 15);
    local_filename[15] = '\0';

    // Free the memory the shell allocated for this string
    kfree(passed_filename);
    task_list[current_task_idx].code_ptr = NULL;
  } else {
    kstrcpy(local_filename, "UNTITLED.TXT"); // Fallback
  }

  char *text_buffer = (char *)kmalloc(4096);
  if (!text_buffer) {
    // Exit via syscall if allocation fails
    __asm__ volatile("mov $4, %eax; int $0x80");
    while (1)
      yield();
  }
  kmemset(text_buffer, 0, 4096 / 4);

  // 2. LAYOUT SYNCHRONIZATION
  // Wait for the Window Manager/Compositor to assign a tiled width.
  // This prevents the 'rep stosl' below from clearing 0 bytes or out-of-bounds.
  while (task_list[current_task_idx].win_w < 10) {
    yield();
  }

  // 3. FILE LOADING
  vfs_node_t *node = vfs_walk_path(local_filename, NULL);
  uint32_t total_len = 0;

  if (node && !(node->flags & FS_DIRECTORY)) {

    uint32_t read_len = vfs_read(node, 0, node->size, (uint8_t *)text_buffer);
    total_len = (read_len > 4094) ? 4094 : read_len;

    kfree(node);

  } else {
    // file doesn't exist → create it
    if (node)
      kfree(node);
    vfs_create(fs_current_dir, local_filename, FS_FILE);
  }

  uint32_t cursor_index = total_len;
  int needs_redraw = 1;
  int last_cursor_state = -1;

  // 4. MAIN INTERACTIVE LOOP
  while (1) {
    int tid = get_current_task_id();
    struct task *t = (struct task *)&task_list[tid];

    // Ensure we stop if the task was killed via 'kill <tid>' from shell
    if (t->state == 0)
      break;

    // Safety: ensure window is fully registered
    if (!t->has_window || !t->window_ready) {
      yield();
      continue;
    }

    // --- INPUT HANDLING (Direct Buffer Read) ---
    if (t->kbd_head != t->kbd_tail) {
      char c = t->kbd_buffer[t->kbd_tail];
      t->kbd_tail = (t->kbd_tail + 1) % 64;
      needs_redraw = 1;

      if (c == 17)
        break;       // Ctrl+Q (Exit)
      if (c == 19) { // Ctrl+S (Save)
        vfs_node_t *node = vfs_finddir(fs_current_dir, local_filename);

        if (!node) {
          vfs_create(fs_current_dir, local_filename, FS_FILE);
          node = vfs_finddir(fs_current_dir, local_filename);
        }

        if (node) {
          uint32_t len = kstrlen(text_buffer);
          vfs_write(node, 0, len, (uint8_t *)text_buffer);
          kfree(node);
        }
        continue;
      }

      // Navigation
      if (c == 13 && cursor_index > 0)
        cursor_index--; // Left Arrow
      else if (c == 14 && cursor_index < total_len)
        cursor_index++; // Right Arrow

      // Deletion
      else if (c == '\b' && cursor_index > 0) {
        for (uint32_t i = cursor_index - 1; i < total_len; i++) {
          text_buffer[i] = text_buffer[i + 1];
        }
        cursor_index--;
        total_len--;
      }
      // Insertion
      else if ((c >= ' ' || c == '\n') && total_len < 4094) {
        for (uint32_t i = total_len; i > cursor_index; i--) {
          text_buffer[i] = text_buffer[i - 1];
        }
        text_buffer[cursor_index] = c;
        cursor_index++;
        total_len++;
        text_buffer[total_len] = '\0';
      }
    }

    // --- RENDERING (Blinks on Timer) ---
    int current_cursor_state = (system_ticks / 500) % 2;
    if (current_cursor_state != last_cursor_state) {
      needs_redraw = 1;
      last_cursor_state = current_cursor_state;
    }

    if (needs_redraw) {
      // Hardware-accelerated clear of the private window buffer
      uint32_t count = t->win_w * t->win_h;
      uint32_t val = 0x222222;
      uint32_t *dest = t->window_buffer;
      __asm__ volatile("rep stosl"
                       : "+D"(dest), "+c"(count)
                       : "a"(val)
                       : "memory");

      // Padding and Header
      t->cursor_x = 10;
      t->cursor_y = 10;

      kconsole_write(t, "KED Editor - ", 0xFFFF00);
      kconsole_write(t, local_filename, 0x00FFFF);
      kconsole_write(t, "\nCtrl+S: Save | Ctrl+Q: Exit\n", 0xAAAAAA);
      kconsole_write(t, "---------------------------\n", 0x555555);

      // Print text before cursor
      char saved = text_buffer[cursor_index];
      text_buffer[cursor_index] = '\0';
      kconsole_write(t, text_buffer, 0xFFFFFF);
      text_buffer[cursor_index] = saved;

      int cx = t->cursor_x;
      int cy = t->cursor_y;

      // Draw Block Cursor (Cyan)
      if (current_cursor_state == 0) {
        struct multiboot_info *mbi = VESA_get_boot_info();
        for (int i = 0; i < 10; i++) {
          for (int j = 0; j < 8; j++) {
            int dx = cx + j;
            int dy = cy + i;
            if (dx < (int)t->win_w && dy < (int)t->win_h)
              t->window_buffer[(dy * mbi->framebuffer_width) + dx] = 0x00FFFF;
          }
        }
      }

      // Print character under cursor (Black on Cyan if blink is on)
      char under[2] = {(saved && saved != '\n') ? saved : ' ', '\0'};
      draw_char(t, under[0], cx, cy, current_cursor_state ? 0xFFFFFF : 0x000000,
                0x222222);

      // Print remainder of the text
      if (saved != '\0') {
        if (saved == '\n') {
          t->cursor_x = 10;
          t->cursor_y += 10;
        } else {
          t->cursor_x += 8;
        }
        kconsole_write(t, &text_buffer[cursor_index + 1], 0xFFFFFF);
      }

      // Update the dirty rectangle for the compositor
      mark_task_dirty(current_task_idx, 0, 0, 4000, 4000);
      needs_redraw = 0;
    }
    yield();
  }

  // 5. CLEANUP AND TERMINATION
  kfree(text_buffer);
  __asm__ volatile("mov $4, %eax; int $0x80"); // Task Exit Syscall
  while (1)
    yield();
}
