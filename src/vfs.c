#include "vfs.h"

// Initialize the root node to null
vfs_node_t* fs_root = 0; 
vfs_node_t* fs_current_dir = 0; 

// The generic read function
uint32_t vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    // If the node has a read function attached, call it!
    if (node->read != 0) {
        return node->read(node, offset, size, buffer);
    } else {
        return 0; // Not readable
    }
}

// The generic write function
uint32_t vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (node->write != 0) {
        return node->write(node, offset, size, buffer);
    } else {
        return 0; // Not writable
    }
}

// The generic find directory function
vfs_node_t* vfs_finddir(vfs_node_t* node, char* name) {
    if ((node->flags & FS_DIRECTORY) && node->finddir != 0) {
        return node->finddir(node, name);
    } else {
        return 0; // Not a directory, or doesn't support finding
    }
}
// The ACTUAL function body (Definition)
uint32_t dev_clock_read(struct vfs_node* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    extern uint32_t system_ticks; // Grab your OS's internal hardware timer
    
    // Create a dynamic string in RAM
    char msg[64] = "VIRTUAL CLOCK DEVICE. System Ticks: ";
    char num_str[16];
    itoa(system_ticks, num_str, 10); // Convert ticks to a string
    kstrcpy(msg + 36, num_str);      // Append the number to the message
    
    uint32_t len = kstrlen(msg);
    
    // Copy our RAM string into the VFS buffer
    kmemcpy(buffer, (uint8_t*)msg, len);
    return len; // Tell the VFS how many bytes we "read"
}
