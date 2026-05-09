#ifndef VFS_H
#define VFS_H

#include <stdint.h>

#define FS_FILE        0x01
#define FS_DIRECTORY   0x02
#define FS_MOUNTPOINT  0x08

struct vfs_node;

// Function Pointers (The Interface)
// Any driver (FAT, CD-ROM, etc) must provide functions that look exactly like these.
typedef uint32_t (*read_type_t)(struct vfs_node*, uint32_t offset, uint32_t size, uint8_t* buffer);
typedef uint32_t (*write_type_t)(struct vfs_node*, uint32_t offset, uint32_t size, uint8_t* buffer);
typedef struct vfs_node* (*finddir_type_t)(struct vfs_node*, char* name);

typedef struct vfs_node {
    char name[128];
    uint32_t flags;       // Is it a file, dir, or mountpoint?
    uint32_t size;        
    
    // The driver-specific functions get attached here
    read_type_t read;
    write_type_t write;
    finddir_type_t finddir;
    
    void* device_specific_data; // Used to store FAT cluster # or internal drive data
} vfs_node_t;

extern vfs_node_t* fs_root; // The absolute root of the OS: "/"
extern vfs_node_t* fs_current_dir; // NEW: The current working directory
// Standard OS API (The Shell calls these!)
uint32_t vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer);
uint32_t vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer);
vfs_node_t* vfs_finddir(vfs_node_t* node, char* name);

uint32_t dev_clock_read(struct vfs_node* node, uint32_t offset, uint32_t size, uint8_t* buffer);
vfs_node_t* vfs_walk_path(char* path);
#endif
