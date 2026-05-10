#ifndef WM_H
#define WM_H

#include <stdint.h>
#include "task.h"

// Expose these to the rest of the OS
void compositor_task();
void task_create_window(int tid, int x, int y, int w, int h);
void refresh_tiling_layout();
void mark_task_dirty(int id, int x, int y, int w, int h);

#endif
