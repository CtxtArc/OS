#include "vfs.h"
#include "lib.h"
#include "kheap.h"

// Initialize the root nodes to null
vfs_node_t* fs_root = 0; 
vfs_node_t* fs_current_dir = 0; 

// --- THE DEVFS (RAM FILESYSTEM) ENGINE ---
vfs_node_t devfs_nodes[16]; 
int devfs_count = 0;
vfs_node_t devfs_root;      

// Helper to quickly add new virtual files
void devfs_register(char* name, uint32_t size, void* read_ptr) {
    kstrcpy(devfs_nodes[devfs_count].name, name);
    devfs_nodes[devfs_count].flags = FS_FILE;
    devfs_nodes[devfs_count].size = size;
    devfs_nodes[devfs_count].read = read_ptr;
    devfs_nodes[devfs_count].write = 0;
    devfs_count++;
}

// DevFS Search Function (Searches RAM, not FAT!)
vfs_node_t* devfs_finddir(vfs_node_t* node, char* name) {
    for (int i = 0; i < devfs_count; i++) {
        if (kstrcasecmp(name, devfs_nodes[i].name) == 0) {
            vfs_node_t* out = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
            kmemcpy(out, &devfs_nodes[i], sizeof(vfs_node_t));
            return out;
        }
    }
    return NULL; 
}

// The generic read function
uint32_t vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (node->read != 0) return node->read(node, offset, size, buffer);
    return 0; 
}

// The generic write function
uint32_t vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (node->write != 0) return node->write(node, offset, size, buffer);
    return 0; 
}

// The generic find directory function
vfs_node_t* vfs_finddir(vfs_node_t* node, char* name) {
    if ((node->flags & FS_DIRECTORY) && node->finddir != 0) {
        return node->finddir(node, name);
    } 
    return 0; 
}

// --- VIRTUAL DEVICES ---

// 1. /dev/clock
uint32_t dev_clock_read(struct vfs_node* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    extern uint32_t system_ticks; 
    char msg[64] = "VIRTUAL CLOCK DEVICE. System Ticks: ";
    char num_str[16];
    itoa(system_ticks, num_str, 10); 
    kstrcpy(msg + 36, num_str);      
    uint32_t len = kstrlen(msg);
    kmemcpy(buffer, (uint8_t*)msg, len);
    return len; 
}

// 2. /dev/urandom
static uint32_t random_seed = 12345;
uint32_t k_rand() {
    extern uint32_t system_ticks;
    if (random_seed == 12345) random_seed += system_ticks; 
    random_seed = (random_seed * 1103515245 + 12345) & 0x7FFFFFFF;
    return random_seed;
}

uint32_t dev_urandom_read(struct vfs_node* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    for (uint32_t i = 0; i < size; i++) {
        buffer[i] = 32 + (k_rand() % 95); // Printable ASCII chars
    }
    return size;
}

// --- ROOT HIJACKER ---
vfs_node_t* (*original_root_finddir)(vfs_node_t*, char*);

vfs_node_t* root_overlay_finddir(vfs_node_t* node, char* name) {
    if (kstrcasecmp(name, "dev") == 0) {
        vfs_node_t* out = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
        kmemcpy(out, &devfs_root, sizeof(vfs_node_t));
        return out;
    }
    return original_root_finddir(node, name);
}

void setup_virtual_devices() {
    devfs_count = 0;
    devfs_register("clock", 64, dev_clock_read);
    devfs_register("urandom", 256, dev_urandom_read);
    devfs_register("fps", 32, dev_fps_read); 

    kstrcpy(devfs_root.name, "dev");
    devfs_root.flags = FS_DIRECTORY;
    devfs_root.finddir = devfs_finddir;

    if (fs_root != NULL) {
        original_root_finddir = fs_root->finddir;
        fs_root->finddir = root_overlay_finddir;
    }
}
// The VFS Path Walker (Resolves absolute and relative paths!)
vfs_node_t* vfs_walk_path(char* path) {
    if (!path || path[0] == '\0') return NULL;

    char temp_path[256];
    kstrcpy(temp_path, path);
    char* p = temp_path;

    vfs_node_t* current_dir;
    
    // 1. Determine starting point (Absolute vs Relative)
    if (p[0] == '/') {
        current_dir = fs_root;
        p++; // skip slash
    } else {
        current_dir = fs_current_dir;
    }

    // Skip extra leading slashes
    while (*p == '/') p++;
    
    // If the path was just "/"
    if (*p == '\0') {
        vfs_node_t* out = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
        kmemcpy(out, current_dir, sizeof(vfs_node_t));
        return out;
    }

    vfs_node_t* last_node = NULL;

    // 2. Walk the path components
    while (*p) {
        // Find the end of the current folder/file name
        char* slash = p;
        while (*slash != '/' && *slash != '\0') slash++;
        
        int is_last = (*slash == '\0');
        *slash = '\0'; // Split string

        // Ask the current directory to find the next part
        vfs_node_t* next_node = vfs_finddir(current_dir, p);

        // Clean up the previous dynamic node to prevent memory leaks
        if (last_node) kfree(last_node);

        if (!next_node) return NULL; // Path is broken!

        last_node = next_node;
        current_dir = next_node;

        if (is_last) break;

        // Move pointer to next part of the path
        p = slash + 1;
        while (*p == '/') p++; // skip consecutive slashes
    }

    return last_node;
}
//  /dev/fps
uint32_t dev_fps_read(struct vfs_node* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    extern volatile uint32_t current_fps;
    char msg[32] = "FPS: ";
    char num_str[16];
    
    itoa(current_fps, num_str, 10);
    kstrcat(msg, num_str);
    kstrcat(msg, "\n");
    
    uint32_t len = kstrlen(msg);
    if (offset >= len) return 0;
    
    uint32_t to_read = len - offset;
    if (to_read > size) to_read = size;
    
    kmemcpy(buffer, (uint8_t*)msg + offset, to_read);
    return to_read;
}
