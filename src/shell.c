#include "shell.h"
#include "assembler.h"
#include "kheap.h"
#include "lib.h"
#include "pmm.h"
#include "task.h"
#include "vesa.h"
#include "vfs.h"
#include <stdint.h>

extern volatile struct task task_list[];
extern int vesa_dirty;
extern int vesa_updating;
extern uint32_t system_ticks;
extern uint32_t total_pages;
extern uint32_t timer_frequency;
extern uint32_t target_fps;
extern int keyboard_focus_tid;
extern int current_task_idx;
volatile int shell_tid = -1;

extern void KED_init(const char *filename);
extern void KED_task();
extern void mark_task_dirty(int id, int x, int y, int w, int h);

// =============================================================================
// shell.c — filesystem-related commands are VFS-only.
//
// Single source of truth for the current working directory:
//   fs_current_dir   (vfs_node_t*) — what every vfs_* call actually uses
//   shell_cwd        (char[])      — display string kept in sync by cmd_cd()
//
// No fat_* function is called anywhere below this comment. The only fat_*
// symbol still referenced in this file is fat_vfs_mount(), which belongs in
// kernel init, not here.
// =============================================================================

char shell_cwd[256] = "/";

// ---------------------------------------------------------------------------
// LS — uses vfs_readdir, works on ANY mounted filesystem (FAT or /dev)
// ---------------------------------------------------------------------------
void cmd_ls(const char *arg) {
  vfs_node_t *dir = fs_current_dir;
  vfs_node_t *resolved = NULL;

  if (arg && arg[0] != '\0') {
    resolved = vfs_walk_path(arg, NULL);
    if (!resolved) {
      shell_print_current("LS: path not found.\n", COLOR_RED);
      return;
    }
    if (!(resolved->flags & FS_DIRECTORY)) {
      shell_print_current("LS: not a directory.\n", COLOR_RED);
      kfree(resolved);
      return;
    }
    dir = resolved;
  }

  shell_print_current("Type   Name             Size\n", 0xAAAAAA);
  shell_print_current("----------------------------\n", 0xAAAAAA);

  vfs_node_t entry;
  uint32_t index = 0;
  while (vfs_readdir(dir, index, &entry)) {
    uint32_t color = (entry.flags & FS_DIRECTORY) ? 0x00FFFF : 0xFFFFFF;
    shell_print_current((entry.flags & FS_DIRECTORY) ? "[DIR]  " : "       ",
                        color);
    shell_print_current(entry.name, color);

    int pad = 17 - (int)kstrlen(entry.name);
    char padbuf[20];
    int pi = 0;
    for (int i = 0; i < pad && i < 19; i++)
      padbuf[pi++] = ' ';
    padbuf[pi] = '\0';
    shell_print_current(padbuf, color);

    char szbuf[16];
    itoa(entry.size, szbuf, 10);
    shell_print_current(szbuf, 0x888888);
    shell_print_current(" bytes\n", 0x888888);

    index++;
  }

  if (resolved)
    kfree(resolved);
}

// ---------------------------------------------------------------------------
// CD — resolves the target via vfs_walk_path, updates fs_current_dir +
// shell_cwd
// ---------------------------------------------------------------------------
void cmd_cd(const char *arg) {
  if (!arg || arg[0] == '\0') {
    shell_print_current("Usage: CD <dirname>\n", COLOR_WHITE);
    return;
  }

  vfs_node_t *target = vfs_walk_path(arg, NULL);
  if (!target) {
    shell_print_current("CD: Directory not found.\n", COLOR_RED);
    return;
  }
  if (!(target->flags & FS_DIRECTORY)) {
    shell_print_current("CD: not a directory.\n", COLOR_RED);
    kfree(target);
    return;
  }

  if (fs_current_dir && fs_current_dir != fs_root) {
    kfree(fs_current_dir);
  }
  fs_current_dir = target; // now owns this allocation

  // --- maintain shell_cwd display string ---
  if (arg[0] == '/') {
    kstrcpy(shell_cwd, arg);
    if (shell_cwd[0] == '\0')
      kstrcpy(shell_cwd, "/");
  } else if (kstrcmp(arg, "..") == 0) {
    int len = kstrlen(shell_cwd);
    if (len > 1) {
      for (int i = len - 1; i >= 0; i--) {
        if (shell_cwd[i] == '/') {
          shell_cwd[i] = '\0';
          if (i == 0)
            kstrcpy(shell_cwd, "/");
          break;
        }
      }
    }
  } else if (kstrcmp(arg, ".") != 0) {
    if (kstrcmp(shell_cwd, "/") != 0)
      kstrcat(shell_cwd, "/");
    kstrcat(shell_cwd, arg);
  }
}

void cmd_pwd(void) {
  shell_print_current(shell_cwd, COLOR_WHITE);
  shell_print_current("\n", COLOR_WHITE);
}

// ---------------------------------------------------------------------------
// CAT — read entire file through VFS and print it
// ---------------------------------------------------------------------------
void cmd_cat(const char *filename) {
  vfs_node_t *node = vfs_walk_path(filename, NULL);
  if (!node) {
    shell_print_current("Error: File not found or is a directory.\n",
                        COLOR_RED);
    return;
  }
  if (node->flags & FS_DIRECTORY) {
    shell_print_current("Error: File not found or is a directory.\n",
                        COLOR_RED);
    kfree(node);
    return;
  }

  char *buf = (char *)kmalloc(node->size + 1);
  if (!buf) {
    kfree(node);
    return;
  }

  uint32_t read = vfs_read(node, 0, node->size, (uint8_t *)buf);
  buf[read] = '\0';

  shell_print_current(buf, COLOR_WHITE);
  shell_print_current("\n", COLOR_WHITE);

  kfree(buf);
  kfree(node);
}

// ---------------------------------------------------------------------------
// HEXDUMP — through VFS
// ---------------------------------------------------------------------------
void cmd_hexdump(const char *filename) {
  vfs_node_t *node = vfs_walk_path(filename, NULL);
  if (!node) {
    shell_print_current("HEXDUMP: file not found.\n", COLOR_RED);
    return;
  }
  if (node->flags & FS_DIRECTORY) {
    shell_print_current("HEXDUMP: is a directory.\n", COLOR_RED);
    kfree(node);
    return;
  }
  if (node->size == 0) {
    shell_print_current("File is empty.\n", COLOR_WHITE);
    kfree(node);
    return;
  }

  uint8_t *buf = (uint8_t *)kmalloc(node->size);
  if (buf) {
    uint32_t read = vfs_read(node, 0, node->size, buf);
    kprintf_unsync("Hexdump of %s (%d bytes):\n", filename, read);
    hexdump(buf, read);
    kfree(buf);
  }
  kfree(node);
}

// ---------------------------------------------------------------------------
// TOUCH — create an empty file via vfs_create on the current directory
// ---------------------------------------------------------------------------
void cmd_touch(const char *filename) {
  if (vfs_create(fs_current_dir, (char *)filename, FS_FILE) == 0) {
    kprintf_unsync("Created file: %s\n", filename);
  } else {
    shell_print_current("TOUCH: could not create file.\n", COLOR_RED);
  }
}

// ---------------------------------------------------------------------------
// WRITE — create-if-needed, then vfs_write the full contents
// ---------------------------------------------------------------------------
void cmd_write(const char *filename, const char *data) {
  vfs_node_t *node = vfs_finddir(fs_current_dir, (char *)filename);
  if (!node) {
    if (vfs_create(fs_current_dir, (char *)filename, FS_FILE) != 0) {
      shell_print_current("WRITE: could not create file.\n", COLOR_RED);
      return;
    }
    node = vfs_finddir(fs_current_dir, (char *)filename);
    if (!node) {
      shell_print_current("WRITE: internal error.\n", COLOR_RED);
      return;
    }
  }

  uint32_t len = kstrlen(data);
  uint32_t written = vfs_write(node, 0, len, (uint8_t *)data);
  kprintf_unsync("Saved %d bytes to %s\n", written, filename);
  kfree(node);
}

// ---------------------------------------------------------------------------
// MKDIR / RM / RMDIR — passthroughs to vfs_mkdir / vfs_unlink / vfs_rmdir
// ---------------------------------------------------------------------------
void cmd_mkdir(const char *dirname) {
  if (vfs_mkdir(fs_current_dir, (char *)dirname) == 0) {
    kprintf_unsync("Directory '%s' created.\n", dirname);
  } else {
    shell_print_current("MKDIR: failed.\n", COLOR_RED);
  }
}

void cmd_rm(const char *filename) {
  if (vfs_unlink(fs_current_dir, (char *)filename) == 0) {
    kprintf_unsync("File '%s' removed.\n", filename);
  } else {
    shell_print_current("RM: failed (not found, or is a directory).\n",
                        COLOR_RED);
  }
}

void cmd_rmdir(const char *dirname) {
  if (vfs_rmdir(fs_current_dir, (char *)dirname) == 0) {
    kprintf_unsync("Directory '%s' removed.\n", dirname);
  } else {
    shell_print_current("RMDIR: failed.\n", COLOR_RED);
  }
}

// ---------------------------------------------------------------------------
char *find_space(char *str) {
  while (*str) {
    if (*str == ' ')
      return str;
    str++;
  }
  return 0;
}

void dummy_app() {
  __asm__ volatile("sti");
  while (1) {
    yield();
    volatile char *vga = (char *)0xB8000;
    vga[0]++;
  }
}

// ---------------------------------------------------------------------------
// RUN — load a binary's bytes through VFS, then hand off to the scheduler
// ---------------------------------------------------------------------------
static void cmd_run(char *arg) {
  if (!arg) {
    shell_print_current("Usage: RUN <file> [args]\n", COLOR_WHITE);
    return;
  }

  char filename[32];
  char *app_args = NULL;
  int k = 0;

  while (arg[k] != ' ' && arg[k] != '\0' && k < 31) {
    filename[k] = arg[k];
    k++;
  }
  filename[k] = '\0';

  if (arg[k] == ' ') {
    app_args = &arg[k + 1];
    while (*app_args == ' ')
      app_args++;
  }

  vfs_node_t *node = vfs_walk_path(filename, NULL);
  if (!node || (node->flags & FS_DIRECTORY)) {
    shell_print_current("File not found.\n", COLOR_RED);
    if (node)
      kfree(node);
    return;
  }

  uint32_t size = node->size;
  void *raw_code = kmalloc(size + 8192); // extra space for variables/args
  if (!raw_code) {
    shell_print_current("RUN: out of memory.\n", COLOR_RED);
    kfree(node);
    return;
  }

  uint32_t aligned_code = ((uint32_t)raw_code + 0xFFF) & 0xFFFFF000;
  vfs_read(node, 0, size, (uint8_t *)aligned_code);
  kfree(node);

  // code_ptr MUST be the raw kmalloc pointer for task_set_arguments to work
  int tid = spawn_task((void (*)())aligned_code, raw_code, filename);

  if (tid >= 0) {
    if (app_args && *app_args != '\0') {
      task_set_arguments(tid, app_args);
    }
    task_create_window(tid, 0, 0, 0, 0);
  } else {
    kfree(raw_code);
  }
}

// ---------------------------------------------------------------------------
// COMPILE — source read through VFS, binary written through VFS
// ---------------------------------------------------------------------------
void shell_compile(const char *arg) {
  vfs_node_t *file = vfs_walk_path(arg, NULL);
  if (!file || (file->flags & FS_DIRECTORY)) {
    kprintf_unsync("Error: %s not found\n", arg);
    if (file)
      kfree(file);
    return;
  }

  char *source_buf = (char *)kmalloc(file->size + 1);
  uint32_t src_len = vfs_read(file, 0, file->size, (uint8_t *)source_buf);
  source_buf[src_len] = '\0';
  kfree(file);

  uint8_t *binary_buf = (uint8_t *)kmalloc(8192);

  label_count = 0;
  rt_var_count = 0;

  uint32_t binary_size = 0;

  for (int pass = 1; pass <= 2; pass++) {
    uint32_t current_pos = 0;
    uint32_t binary_pos = 0;

    while (current_pos < kstrlen(source_buf)) {
      char temp_line[128];
      uint32_t i = 0;

      while (current_pos < kstrlen(source_buf) &&
             source_buf[current_pos] != '\n' && i < 127) {
        temp_line[i++] = source_buf[current_pos++];
      }
      temp_line[i] = '\0';
      current_pos++;

      char *walk = temp_line;
      while (*walk == ' ' || *walk == '\t')
        walk++;
      if (*walk == '#' || *walk == '\0')
        continue;

      if (pass == 1) {
        assemble_line(temp_line, NULL, &binary_pos, 1);
      } else {
        assemble_line(temp_line, binary_buf, &binary_pos, 2);
      }
    }

    if (pass == 2)
      binary_size = binary_pos;
  }

  // Build output filename: NAME.BIN
  char out_name[16];
  int k;
  for (k = 0; k < 11 && arg[k] != '.' && arg[k] != '\0'; k++)
    out_name[k] = arg[k];
  out_name[k++] = '.';
  out_name[k++] = 'B';
  out_name[k++] = 'I';
  out_name[k++] = 'N';
  out_name[k] = '\0';

  // Write the compiled binary through VFS
  vfs_node_t *out_node = vfs_finddir(fs_current_dir, out_name);
  if (!out_node) {
    vfs_create(fs_current_dir, out_name, FS_FILE);
    out_node = vfs_finddir(fs_current_dir, out_name);
  }
  if (out_node) {
    vfs_write(out_node, 0, binary_size, binary_buf);
    kfree(out_node);
    kprintf_unsync("Compiled %s to %s (%d bytes)\n", arg, out_name,
                   binary_size);
  } else {
    kprintf_unsync("Compile Error: could not create %s\n", out_name);
  }

  kfree(source_buf);
  kfree(binary_buf);
}

// ---------------------------------------------------------------------------
// execute_command — dispatcher. Filesystem branches call cmd_* only.
// ---------------------------------------------------------------------------
void execute_command(char *input) {
  char *arg = find_space(input);
  if (arg) {
    *arg = '\0';
    arg++;
  }

  // --- SYSTEM & INFO ---
  if (kstrcasecmp(input, "HELP") == 0) {
    shell_print_current(
        "Commands: LS CD CAT MKDIR RM RMDIR PWD TOUCH CLEAR PS KILL "
        "SLEEP RUN TOP UPTIME STAT REBOOT CRASH ECHO SET_FPS TIMER "
        "GAME HEXDUMP WRITE COMPILE KED\n",
        COLOR_WHITE);
  } else if (kstrcasecmp(input, "SUICIDE") == 0) {
    shell_print_current("Spawning SUICIDE task... watch the logs.\n",
                        COLOR_WHITE);
    spawn_task(suicide_task, NULL, "suicide_app");

  } else if (kstrcasecmp(input, "CAT") == 0) {
    if (arg)
      cmd_cat(arg);
    else
      shell_print_current("Usage: CAT <file>\n", COLOR_WHITE);

  } else if (kstrcasecmp(input, "LS") == 0) {
    cmd_ls(arg);

  } else if (kstrcasecmp(input, "CD") == 0) {
    cmd_cd(arg);

  } else if (kstrcasecmp(input, "PWD") == 0) {
    cmd_pwd();

  } else if (kstrcasecmp(input, "MKDIR") == 0) {
    if (arg)
      cmd_mkdir(arg);
    else
      shell_print_current("Usage: MKDIR <name>\n", COLOR_WHITE);

  } else if (kstrcasecmp(input, "RM") == 0) {
    if (arg)
      cmd_rm(arg);
    else
      shell_print_current("Usage: RM <filename>\n", COLOR_WHITE);

  } else if (kstrcasecmp(input, "RMDIR") == 0) {
    if (arg)
      cmd_rmdir(arg);
    else
      shell_print_current("Usage: RMDIR <dirname>\n", COLOR_WHITE);

  } else if (kstrcasecmp(input, "TOUCH") == 0) {
    if (arg)
      cmd_touch(arg);
    else
      shell_print_current("Usage: TOUCH <filename>\n", COLOR_WHITE);

  } else if (kstrcasecmp(input, "HEXDUMP") == 0) {
    if (arg)
      cmd_hexdump(arg);
    else
      shell_print_current("Usage: HEXDUMP <filename>\n", COLOR_RED);

  } else if (kstrcasecmp(input, "WRITE") == 0) {
    if (arg && kstrlen(arg) > 0) {
      char *filename = arg;
      char *content = NULL;

      for (int i = 0; arg[i] != '\0'; i++) {
        if (arg[i] == ' ') {
          arg[i] = '\0';
          content = &arg[i + 1];
          break;
        }
      }

      if (filename && content) {
        cmd_write(filename, content);
      } else {
        shell_print_current("Usage: WRITE <file> <text>\n", COLOR_RED);
      }
    } else {
      shell_print_current("Usage: WRITE <file> <text>\n", COLOR_RED);
    }

  } else if (kstrcasecmp(input, "PS") == 0) {
    shell_print_current("TID    NAME          STATE\n", COLOR_CYAN);
    for (int i = 0; i < MAX_TASKS; i++) {
      if (task_list[i].state != 0) {
        char buf[16];
        itoa(i, buf, 10);
        shell_print_current(buf, COLOR_WHITE);
        shell_print_current("      ", COLOR_WHITE);
        shell_print_current((char *)task_list[i].name, COLOR_WHITE);
        int len = kstrlen((char *)task_list[i].name);
        for (int j = 0; j < (13 - len); j++)
          shell_print_current(" ", COLOR_WHITE);
        if (task_list[i].state == 1)
          shell_print_current("READY\n", COLOR_GREEN);
        else if (task_list[i].state == 2)
          shell_print_current("SLEEP\n", COLOR_YELLOW);
        else if (task_list[i].state == 3)
          shell_print_current("BLOCKED\n", COLOR_RED);
      }
    }

  } else if (kstrcasecmp(input, "UPTIME") == 0) {
    char buf[32];
    uint32_t s = system_ticks / 1000;
    shell_print_current("Uptime: ", COLOR_WHITE);
    itoa(s, buf, 10);
    shell_print_current(buf, COLOR_CYAN);
    shell_print_current("s\n", COLOR_WHITE);

  } else if (kstrcasecmp(input, "STAT") == 0) {
    shell_print_current("--- KERNEL HEAP STATISTICS ---\n", COLOR_CYAN);
    extern header_t *heap_start;
#define HEAP_INITIAL_SIZE (64 * 1024 * 1024)

    if (!heap_start) {
      shell_print_current("Heap is not initialized.\n", COLOR_RED);
    } else {
      uint32_t free_mem = 0;
      uint32_t used_mem = 0;
      uint32_t blocks = 0;

      header_t *curr = heap_start;
      uint32_t heap_limit = (uint32_t)heap_start + HEAP_INITIAL_SIZE;

      while (curr != NULL) {
        if ((uint32_t)curr < (uint32_t)heap_start ||
            (uint32_t)curr >= heap_limit) {
          shell_print_current("Error: Heap linked-list corrupted!\n",
                              COLOR_RED);
          break;
        }
        if (curr->is_free)
          free_mem += curr->size;
        else
          used_mem += curr->size;
        blocks++;
        if (curr->size == 0 && curr->next != NULL) {
          shell_print_current("Error: Zero-size block detected.\n", COLOR_RED);
          break;
        }
        curr = curr->next;
      }

      char buf[32];
      shell_print_current("Total Blocks: ", COLOR_WHITE);
      itoa(blocks, buf, 10);
      shell_print_current(buf, COLOR_YELLOW);
      shell_print_current("\n", COLOR_WHITE);

      shell_print_current("Used Memory:  ", COLOR_WHITE);
      itoa(used_mem / 1024, buf, 10);
      shell_print_current(buf, COLOR_YELLOW);
      shell_print_current(" KB\n", COLOR_WHITE);

      shell_print_current("Free Memory:  ", COLOR_WHITE);
      itoa(free_mem / 1024, buf, 10);
      shell_print_current(buf, COLOR_GREEN);
      shell_print_current(" KB\n", COLOR_WHITE);

      if (free_mem < 4194304) {
        shell_print_current(
            "WARNING: Low Heap Memory! Close windows or REBOOT.\n", COLOR_RED);
      }
    }

  } else if (kstrcasecmp(input, "CLEAR") == 0) {
    int sw = task_list[current_task_idx].win_w;
    int sh = task_list[current_task_idx].win_h;
    for (int i = 0; i < sw * sh; i++) {
      task_list[current_task_idx].window_buffer[i] = 0x222222;
    }
    task_list[current_task_idx].cursor_x = 2;
    task_list[current_task_idx].cursor_y = 2;

  } else if (kstrcasecmp(input, "ECHO") == 0) {
    if (arg) {
      shell_print_current(arg, COLOR_WHITE);
      shell_print_current("\n", COLOR_WHITE);
    }

  } else if (kstrcasecmp(input, "SLEEP") == 0) {
    if (arg)
      sleep(katoi(arg));

  } else if (kstrcasecmp(input, "KED") == 0) {
    if (arg) {
      char *fn = kmalloc(16);
      kstrncpy(fn, arg, 15);
      fn[15] = '\0';
      int tid = spawn_task(KED_task, fn, "KED");
      task_create_window(tid, 0, 0, 0, 0);
      keyboard_focus_tid = tid;
      shell_print_current("Text Editor Spawned.\n", COLOR_GREEN);
    } else {
      shell_print_current("Usage: KED <filename>\n", COLOR_RED);
    }

  } else if (kstrcasecmp(input, "TOP") == 0) {
    extern void run_top();
    int tid = spawn_task(run_top, NULL, "TOP");
    task_create_window(tid, 0, 0, 0, 0);
    shell_print_current("TOP Spawned.\n", COLOR_GREEN);

  } else if (kstrcmp(input, "GAME") == 0) {
    extern void task_game();
    int tid = spawn_task(task_game, NULL, "GAME");
    task_create_window(tid, 0, 0, 0, 0);
    shell_print_current("Game Spawned.\n", COLOR_GREEN);

  } else if (kstrcmp(input, "TIMER") == 0) {
    extern void task_timer();
    spawn_task(task_timer, NULL, "TIMER");
    shell_print_current("Background Timer Spawned.\n", COLOR_GREEN);

  } else if (kstrcasecmp(input, "RUN") == 0) {
    cmd_run(arg);

  } else if (kstrcasecmp(input, "COMPILE") == 0) {
    if (arg)
      shell_compile(arg);
    else
      shell_print_current("Usage: COMPILE <file.txt>\n", COLOR_RED);

  } else if (kstrcasecmp(input, "KILL") == 0) {
    if (arg) {
      int id = katoi(arg);
      if (id == 0)
        shell_print_current("Cannot kill Shell.\n", COLOR_RED);
      else {
        kill_task(id);
        shell_print_current("Task killed.\n", COLOR_GREEN);
      }
    }

  } else if (input[0] != '\0') {
    shell_print_current("Unknown command: ", COLOR_RED);
    shell_print_current(input, COLOR_RED);
    shell_print_current("\n", COLOR_RED);
  }

  mark_task_dirty(current_task_idx, 0, 0, 4000, 4000);
}

void shell_print_current(char *str, uint32_t color) {
  int tid = get_current_task_id();
  struct task *t = &task_list[tid];
  shell_print(t, str, color);
}

// Always target TID 0 (The Kernel/Shell task) for terminal output
#define SHELL_TID 0

void shell_print(struct task *t, char *str, uint32_t color) {
  if (!t || !t->window_buffer)
    return;

  int padding_x = WIN_BORDER + 2;

  for (int i = 0; str[i] != '\0'; i++) {

    if (str[i] == '\n') {
      t->cursor_x = padding_x;
      t->cursor_y += 10;

      if (t->cursor_y + 10 >= t->win_h)
        shell_scroll(t);

      continue;
    }

    shell_draw_char(str[i], t->cursor_x, t->cursor_y, color, 0x222222);

    t->cursor_x += 8;

    if (t->cursor_x >= t->win_w - 8) {
      t->cursor_x = padding_x;
      t->cursor_y += 10;

      if (t->cursor_y + 10 >= t->win_h)
        shell_scroll(t);
    }
  }

  mark_task_dirty(get_current_task_id(), 0, 0, t->win_w, t->win_h);
}
void shell_draw_char(char c, int x, int y, uint32_t fg, uint32_t bg) {
  if (shell_tid < 0)
    return;
  volatile struct task *t = &task_list[shell_tid];
  struct multiboot_info *mbi = VESA_get_boot_info();
  uint32_t sw = mbi->framebuffer_width;

  if (!t->has_window || !t->window_buffer)
    return;

  extern uint8_t font8x8_basic[128][8];
  uint8_t *glyph = font8x8_basic[(unsigned char)c];
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      int dx = x + col;
      int dy = y + row;
      if (dx >= 0 && dx < t->win_w && dy >= 0 && dy < t->win_h) {
        if (glyph[row] & (1 << (7 - col)))
          t->window_buffer[(dy * sw) + dx] = fg;
        else
          t->window_buffer[(dy * sw) + dx] = bg;
      }
    }
  }
}

void shell_scroll(struct task *t) {
  if (!t || !t->window_buffer)
    return;

  struct multiboot_info *mbi = VESA_get_boot_info();
  uint32_t sw = mbi->framebuffer_width;
  int scroll_y = 10;

  for (int y = 0; y < t->win_h - scroll_y; y++) {
    uint32_t *dst = &t->window_buffer[y * sw];
    uint32_t *src = &t->window_buffer[(y + scroll_y) * sw];

    __asm__ volatile("rep movsl"
                     : "+D"(dst), "+S"(src), "+c"(t->win_w)
                     :
                     : "memory");
  }

  for (int y = t->win_h - scroll_y; y < t->win_h; y++) {
    uint32_t *dst = &t->window_buffer[y * sw];
    uint32_t val = 0x222222;

    __asm__ volatile("rep stosl"
                     : "+D"(dst), "+c"(t->win_w)
                     : "a"(val)
                     : "memory");
  }

  t->cursor_y -= scroll_y;

  mark_task_dirty(get_current_task_id(), 0, 0, t->win_w, t->win_h);
}
