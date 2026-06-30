// vfs.c — Virtual File System core
// All kernel subsystems and the shell go through these functions.
// No FAT or devfs symbols should appear outside this file and their adapters.

#include "vfs.h"
#include "kheap.h"
#include "lib.h"

// ---------------------------------------------------------------
// Global state
// ---------------------------------------------------------------
vfs_node_t *fs_root = 0;
vfs_node_t *fs_current_dir = 0;

// ---------------------------------------------------------------
// Core VFS wrappers
// Each checks that the function pointer is populated before calling.
// ---------------------------------------------------------------

uint32_t vfs_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                  uint8_t *buffer) {
  if (node && node->read)
    return node->read(node, offset, size, buffer);
  return 0;
}

uint32_t vfs_write(vfs_node_t *node, uint32_t offset, uint32_t size,
                   uint8_t *buffer) {
  if (node && node->write)
    return node->write(node, offset, size, buffer);
  return 0;
}

int vfs_truncate(vfs_node_t *node, uint32_t new_size) {
  if (node && node->truncate)
    return node->truncate(node, new_size);
  return -1;
}

// Returns a heap-allocated node; caller must kfree() it.
vfs_node_t *vfs_finddir(vfs_node_t *node, char *name) {
  if (node && (node->flags & FS_DIRECTORY) && node->finddir)
    return node->finddir(node, name);
  return 0;
}

// Fills *out with the entry at index; returns 1 on success, 0 at end.
int vfs_readdir(vfs_node_t *node, uint32_t index, vfs_node_t *out) {
  if (node && (node->flags & FS_DIRECTORY) && node->readdir)
    return node->readdir(node, index, out);
  return 0;
}

int vfs_create(vfs_node_t *parent, char *name, uint32_t flags) {
  if (parent && (parent->flags & FS_DIRECTORY) && parent->create)
    return parent->create(parent, name, flags);
  return -1;
}

int vfs_unlink(vfs_node_t *parent, char *name) {
  if (parent && (parent->flags & FS_DIRECTORY) && parent->unlink)
    return parent->unlink(parent, name);
  return -1;
}

int vfs_mkdir(vfs_node_t *parent, char *name) {
  if (parent && (parent->flags & FS_DIRECTORY) && parent->mkdir)
    return parent->mkdir(parent, name);
  return -1;
}

int vfs_rmdir(vfs_node_t *parent, char *name) {
  if (parent && (parent->flags & FS_DIRECTORY) && parent->rmdir)
    return parent->rmdir(parent, name);
  return -1;
}

// ---------------------------------------------------------------
// Path walker
//
// Resolves absolute ("/a/b/c") and relative ("b/c") paths.
// Returns a heap-allocated vfs_node_t* or NULL on failure.
// If parent_out is non-NULL, *parent_out receives a heap-allocated
// copy of the last directory node traversed before the final component
// (i.e. the parent of the returned node).  Caller must kfree() both.
// ---------------------------------------------------------------
vfs_node_t *vfs_walk_path(const char *path, vfs_node_t **parent_out) {
  if (!path || path[0] == '\0')
    return NULL;
  if (parent_out)
    *parent_out = NULL;

  // Work on a mutable copy so we can NUL-terminate components.
  char temp[256];
  kstrncpy(temp, path, sizeof(temp) - 1);
  temp[sizeof(temp) - 1] = '\0';
  char *p = temp;

  // --- Determine starting node ---
  vfs_node_t *current;
  if (p[0] == '/') {
    current = fs_root;
    p++;
  } else {
    current = fs_current_dir;
  }
  while (*p == '/')
    p++; // eat extra leading slashes

  // Shortcut: path was just "/"
  if (*p == '\0') {
    vfs_node_t *out = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    kmemcpy(out, current, sizeof(vfs_node_t));
    return out;
  }

  // --- Walk each component ---
  // We track the previous node so we can hand it to parent_out.
  vfs_node_t *last_allocated =
      NULL; // the node we got from vfs_finddir last iteration
  vfs_node_t *parent_node = NULL; // one level up from current

  while (*p) {
    // Isolate the next path component
    char *slash = p;
    while (*slash != '/' && *slash != '\0')
      slash++;
    int is_last = (*slash == '\0');
    *slash = '\0';

    // Handle "." — stay put
    if (kstrcmp(p, ".") == 0) {
      if (is_last) {
        // Return a copy of current
        vfs_node_t *out = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
        kmemcpy(out, current, sizeof(vfs_node_t));
        if (parent_out)
          *parent_out = parent_node;
        else if (parent_node && parent_node != fs_root &&
                 parent_node != fs_current_dir)
          kfree(parent_node);
        return out;
      }
      p = slash + 1;
      while (*p == '/')
        p++;
      continue;
    }

    // Handle ".." — go up one level (we can't go above root)
    if (kstrcmp(p, "..") == 0) {
      // We don't track a full parent chain, so ask the current dir
      // for its own ".." entry the same way we'd resolve any name.
      // If current has no finddir or has no ".." entry, stay put.
    }

    // Ask current directory to find this component
    vfs_node_t *next = vfs_finddir(current, p);

    // Free the previous dynamically-allocated node (not the static globals)
    if (last_allocated && last_allocated != fs_root &&
        last_allocated != fs_current_dir) {
      // Don't free it if it is also parent_node we may still need
      if (last_allocated != parent_node) {
        kfree(last_allocated);
      }
    }

    if (!next) {
      // Path broken — clean up and bail
      if (parent_out)
        *parent_out = NULL;
      if (parent_node && parent_node != fs_root &&
          parent_node != fs_current_dir)
        kfree(parent_node);
      return NULL;
    }

    // Rotate: parent becomes current, current becomes next
    if (parent_node && parent_node != fs_root && parent_node != fs_current_dir)
      kfree(parent_node);
    parent_node = last_allocated; // the node we are descending *from*
    last_allocated = next;
    current = next;

    if (is_last)
      break;

    p = slash + 1;
    while (*p == '/')
      p++;
  }

  // last_allocated is the resolved node
  if (parent_out)
    *parent_out = parent_node;
  else if (parent_node && parent_node != fs_root &&
           parent_node != fs_current_dir)
    kfree(parent_node);

  return last_allocated;
}

// ---------------------------------------------------------------
// Convenience path helpers — allocate, operate, free.
// ---------------------------------------------------------------
uint32_t vfs_read_path(const char *path, uint32_t offset, uint32_t size,
                       uint8_t *buf) {
  vfs_node_t *node = vfs_walk_path(path, NULL);
  if (!node)
    return 0;
  uint32_t r = vfs_read(node, offset, size, buf);
  kfree(node);
  return r;
}

uint32_t vfs_write_path(const char *path, uint32_t offset, uint32_t size,
                        uint8_t *buf) {
  vfs_node_t *node = vfs_walk_path(path, NULL);
  if (!node)
    return 0;
  uint32_t w = vfs_write(node, offset, size, buf);
  kfree(node);
  return w;
}

// ---------------------------------------------------------------
// DevFS — a small in-RAM directory mounted at /dev
// ---------------------------------------------------------------

#define DEVFS_MAX 16
static vfs_node_t devfs_nodes[DEVFS_MAX];
static int devfs_count = 0;
static vfs_node_t devfs_root_node;

static void devfs_register(char *name, uint32_t size, read_type_t rfn) {
  if (devfs_count >= DEVFS_MAX)
    return;
  vfs_node_t *n = &devfs_nodes[devfs_count++];
  kstrcpy(n->name, name);
  n->flags = FS_FILE;
  n->size = size;
  n->read = rfn;
  n->write = 0;
  n->finddir = 0;
  n->readdir = 0;
  n->create = 0;
  n->unlink = 0;
  n->mkdir = 0;
  n->rmdir = 0;
  n->truncate = 0;
  n->device_specific_data = 0;
}

// finddir for /dev — search the static devfs_nodes array
static vfs_node_t *devfs_finddir(vfs_node_t *node, char *name) {
  for (int i = 0; i < devfs_count; i++) {
    if (kstrcasecmp(name, devfs_nodes[i].name) == 0) {
      vfs_node_t *out = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
      kmemcpy(out, &devfs_nodes[i], sizeof(vfs_node_t));
      return out;
    }
  }
  return NULL;
}

// readdir for /dev — enumerate by index
static int devfs_readdir(vfs_node_t *node, uint32_t index, vfs_node_t *out) {
  if ((int)index >= devfs_count)
    return 0;
  kmemcpy(out, &devfs_nodes[index], sizeof(vfs_node_t));
  return 1;
}

// --- Root overlay: intercepts "dev" before forwarding to the real FAT root ---
static finddir_type_t original_root_finddir = 0;

static vfs_node_t *root_overlay_finddir(vfs_node_t *node, char *name) {
  if (kstrcasecmp(name, "dev") == 0) {
    vfs_node_t *out = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    kmemcpy(out, &devfs_root_node, sizeof(vfs_node_t));
    return out;
  }
  if (original_root_finddir)
    return original_root_finddir(node, name);
  return NULL;
}

// --- Virtual device read implementations ---

uint32_t dev_clock_read(struct vfs_node *node, uint32_t offset, uint32_t size,
                        uint8_t *buffer) {
  extern uint32_t system_ticks;
  char msg[64];
  kstrcpy(msg, "VIRTUAL CLOCK DEVICE. System Ticks: ");
  char num[16];
  itoa(system_ticks, num, 10);
  kstrcat(msg, num);
  kstrcat(msg, "\n");
  uint32_t len = kstrlen(msg);
  if (offset >= len)
    return 0;
  uint32_t to_read = len - offset;
  if (to_read > size)
    to_read = size;
  kmemcpy(buffer, (uint8_t *)msg + offset, to_read);
  return to_read;
}

static uint32_t random_seed = 12345;
static uint32_t k_rand(void) {
  extern uint32_t system_ticks;
  if (random_seed == 12345)
    random_seed += system_ticks;
  random_seed = (random_seed * 1103515245u + 12345u) & 0x7FFFFFFF;
  return random_seed;
}

uint32_t dev_urandom_read(struct vfs_node *node, uint32_t offset, uint32_t size,
                          uint8_t *buffer) {
  for (uint32_t i = 0; i < size; i++)
    buffer[i] = 32 + (k_rand() % 95); // printable ASCII
  return size;
}

uint32_t dev_fps_read(struct vfs_node *node, uint32_t offset, uint32_t size,
                      uint8_t *buffer) {
  extern volatile uint32_t current_fps;
  char msg[32];
  kstrcpy(msg, "FPS: ");
  char num[16];
  itoa(current_fps, num, 10);
  kstrcat(msg, num);
  kstrcat(msg, "\n");
  uint32_t len = kstrlen(msg);
  if (offset >= len)
    return 0;
  uint32_t to_read = len - offset;
  if (to_read > size)
    to_read = size;
  kmemcpy(buffer, (uint8_t *)msg + offset, to_read);
  return to_read;
}

// Call this once after fs_root has been set up by the FAT adapter.
void vfs_setup_devfs(void) {
  devfs_count = 0;
  devfs_register("clock", 64, dev_clock_read);
  devfs_register("urandom", 256, dev_urandom_read);
  devfs_register("fps", 32, dev_fps_read);

  kstrcpy(devfs_root_node.name, "dev");
  devfs_root_node.flags = FS_DIRECTORY;
  devfs_root_node.size = devfs_count;
  devfs_root_node.finddir = devfs_finddir;
  devfs_root_node.readdir = devfs_readdir;
  devfs_root_node.read = 0;
  devfs_root_node.write = 0;
  devfs_root_node.create = 0;
  devfs_root_node.unlink = 0;
  devfs_root_node.mkdir = 0;
  devfs_root_node.rmdir = 0;
  devfs_root_node.truncate = 0;

  if (fs_root) {
    original_root_finddir = fs_root->finddir;
    fs_root->finddir = root_overlay_finddir;
  }
}
