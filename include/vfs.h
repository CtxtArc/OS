#ifndef VFS_H
#define VFS_H

#include <stdint.h>

// --- Node type flags ---
#define FS_FILE 0x01
#define FS_DIRECTORY 0x02
#define FS_MOUNTPOINT 0x08

struct vfs_node;

// ---------------------------------------------------------------
// Function pointer typedefs — every driver must implement these.
// NULL means "operation not supported" for that node.
// ---------------------------------------------------------------
typedef uint32_t (*read_type_t)(struct vfs_node *, uint32_t offset,
                                uint32_t size, uint8_t *buffer);
typedef uint32_t (*write_type_t)(struct vfs_node *, uint32_t offset,
                                 uint32_t size, uint8_t *buffer);

// finddir: resolve a single path component inside a directory node.
// Returns a heap-allocated vfs_node_t* the caller must kfree(), or NULL.
typedef struct vfs_node *(*finddir_type_t)(struct vfs_node *, char *name);

// readdir: fill `out` with the Nth entry (index 0-based).
// Returns 1 on success, 0 when the index is past the last entry.
// The driver writes into `out`; caller owns the buffer.
typedef int (*readdir_type_t)(struct vfs_node *, uint32_t index,
                              struct vfs_node *out);

// create / unlink / mkdir / rmdir all operate on the *parent* directory node.
typedef int (*create_type_t)(struct vfs_node *parent, char *name,
                             uint32_t flags);
typedef int (*unlink_type_t)(struct vfs_node *parent, char *name);
typedef int (*mkdir_type_t)(struct vfs_node *parent, char *name);
typedef int (*rmdir_type_t)(struct vfs_node *parent, char *name);

// truncate: resize a file to `new_size` bytes.
typedef int (*truncate_type_t)(struct vfs_node *, uint32_t new_size);

// ---------------------------------------------------------------
// The VFS node — one per file, directory, or virtual device.
// ---------------------------------------------------------------
typedef struct vfs_node {
  char name[128];
  uint32_t flags; // FS_FILE | FS_DIRECTORY | FS_MOUNTPOINT
  uint32_t size;  // Byte size (files); entry count hint (dirs, may be 0)

  // Core I/O
  read_type_t read;
  write_type_t write;

  // Directory ops
  finddir_type_t finddir; // Resolve one path component
  readdir_type_t readdir; // Enumerate entries by index

  // Mutation ops
  create_type_t create;     // Create a file inside this directory
  unlink_type_t unlink;     // Delete a file inside this directory
  mkdir_type_t mkdir;       // Create a subdirectory inside this directory
  rmdir_type_t rmdir;       // Remove a subdirectory inside this directory
  truncate_type_t truncate; // Resize this file

  // Driver private data (FAT cluster #, device handle, etc.)
  void *device_specific_data;
} vfs_node_t;

// ---------------------------------------------------------------
// Global state
// ---------------------------------------------------------------
extern vfs_node_t *fs_root;        // Absolute root: "/"
extern vfs_node_t *fs_current_dir; // CWD (used for relative paths)

// ---------------------------------------------------------------
// Public VFS API — the shell and all kernel code call ONLY these.
// Never call FAT / devfs functions directly outside their adapters.
// ---------------------------------------------------------------

// Basic I/O
uint32_t vfs_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                  uint8_t *buffer);
uint32_t vfs_write(vfs_node_t *node, uint32_t offset, uint32_t size,
                   uint8_t *buffer);
int vfs_truncate(vfs_node_t *node, uint32_t new_size);

// Directory operations
vfs_node_t *vfs_finddir(vfs_node_t *node, char *name);
int vfs_readdir(vfs_node_t *node, uint32_t index, vfs_node_t *out);

// Mutation (operate on the *parent* directory node)
int vfs_create(vfs_node_t *parent, char *name, uint32_t flags);
int vfs_unlink(vfs_node_t *parent, char *name);
int vfs_mkdir(vfs_node_t *parent, char *name);
int vfs_rmdir(vfs_node_t *parent, char *name);

// Path resolution — resolves absolute ("/foo/bar") and relative ("bar") paths.
// Returns a heap-allocated vfs_node_t* the caller must kfree(), or NULL.
// The second out-parameter is optional: if non-NULL, *parent_out receives a
// heap-allocated node for the resolved node's parent directory (also kfree()).
vfs_node_t *vfs_walk_path(const char *path, vfs_node_t **parent_out);

// Convenience: resolve path, call vfs_read, kfree the node. Returns bytes read.
uint32_t vfs_read_path(const char *path, uint32_t offset, uint32_t size,
                       uint8_t *buf);
// Convenience: resolve path, call vfs_write, kfree the node. Returns bytes
// written.
uint32_t vfs_write_path(const char *path, uint32_t offset, uint32_t size,
                        uint8_t *buf);

// Virtual device helpers (called only from vfs.c / devfs setup)
uint32_t dev_clock_read(struct vfs_node *, uint32_t, uint32_t, uint8_t *);
uint32_t dev_urandom_read(struct vfs_node *, uint32_t, uint32_t, uint8_t *);
uint32_t dev_fps_read(struct vfs_node *, uint32_t, uint32_t, uint8_t *);

// Mount the devfs overlay onto fs_root; call after fs_root is initialised.
void vfs_setup_devfs(void);

#endif // VFS_H
