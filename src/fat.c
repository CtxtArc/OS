#include "fat.h"
#include <stdint.h>
#include "kheap.h"
#include "io.h"
#include "lib.h"
#include "vesa.h"
#include "vfs.h" 

static struct fat_bpb bpb;
static uint32_t root_dir_sectors;
static uint32_t first_data_sector;
static uint32_t first_fat_sector;
static uint32_t current_dir_cluster = 0; // 0 means Root Directory
static uint8_t global_fat_buf[512] __attribute__((aligned(16)));
static uint8_t raw_io_buffer[512] __attribute__((aligned(16)));
volatile int fat_disk_busy = 0;

unsigned char spinner_code[] = {
    // 1. Get Ticks (Syscall 2)
    0xB8, 0x02, 0x00, 0x00, 0x00, // [0]  MOV EAX, 2
    0xCD, 0x80,                   // [5]  INT 0x80 -> EAX = ticks

    // 2. Slow down and get index (0-3)
    0xC1, 0xE8, 0x05,             // [7]  SHR EAX, 5 (Slow rotation)
    0x83, 0xE0, 0x03,             // [10] AND EAX, 3 (Result is 0, 1, 2, or 3)

    // 3. The "String" Lookup
    // We store the characters "-\|/" in a 32-bit register.
    // ASCII: '-'=0x2D, '\'=0x5C, '|'=0x7C, '/'=0x2F
    0xBB, 0x2D, 0x5C, 0x7C, 0x2F, // [13] MOV EBX, 0x2F7C5C2D

    // 4. Shift EBX right by (EAX * 8) bits to bring the character to BL
    0x88, 0xC1,                   // [18] MOV CL, AL (CL = 0, 1, 2, or 3)
    0xC1, 0xE1, 0x03,             // [20] SHL CL, 3  (CL = 0, 8, 16, or 24)
    0xD3, 0xEB,                   // [23] SHR EBX, CL (Shift chosen char into BL)
    0x81, 0xE3, 0xFF, 0x00, 0x00, 0x00, // [25] AND EBX, 0xFF (Clear upper bits)

    // 5. Print (Syscall 1)
    0xB8, 0x01, 0x00, 0x00, 0x00, // [31] MOV EAX, 1
    0xB9, 0xE8, 0x03, 0x00, 0x00, // [36] MOV ECX, 1010 (X)
    0xBA, 0x05, 0x00, 0x00, 0x00, // [41] MOV EDX, 5  (Y)
    0xCD, 0x80,                   // [46] INT 0x80

    // 6. Yield (FIXED: Replaced INT 0x20 with HLT + NOP)
    0xF4,                         // [48] HLT (Safely sleep until next hardware tick)
    0x90,                         // [49] NOP (Keep byte count at 52 so JMP works)

    // 7. Loop back to index 0
    0xEB, 0xCC                    // [50] JMP -52 bytes
};

unsigned char clock_code[] = {
    // 1. Get Ticks (Syscall 2)
    0xB8, 0x02, 0x00, 0x00, 0x00, // [0]  MOV EAX, 2
    0xCD, 0x80,                   // [5]  INT 0x80 -> EAX = ticks

    // 2. Extract a "Slow" nibble (4 bits)
    // Shifting right by 7 means the number changes every 128 ticks (~1.3s)
    0xC1, 0xE8, 0x07,             // [7]  SHR EAX, 7
    0x83, 0xE0, 0x0F,             // [10] AND EAX, 0x0F (Keep 0-15)

    // 3. Keep it within 0-9 for now
    0xBB, 0x0A, 0x00, 0x00, 0x00, // [13] MOV EBX, 10
    0x31, 0xD2,                   // [18] XOR EDX, EDX
    0xF7, 0xF3,                   // [20] DIV EBX (Remainder in EDX is 0-9)

    // 4. Convert Digit to ASCII
    0x83, 0xC2, 0x30,             // [22] ADD EDX, 0x30
    0x89, 0xD3,                   // [25] MOV EBX, EDX

    // 5. Print (Syscall 1)
    0xB8, 0x01, 0x00, 0x00, 0x00, // [27] MOV EAX, 1
    0xB9, 0xE8, 0x03, 0x00, 0x00, // [32] MOV ECX, 1000 (X pos)
    0xBA, 0x05, 0x00, 0x00, 0x00, // [37] MOV EDX, 5  (Y pos)
    0xCD, 0x80,                   // [42] INT 0x80

    // 6. Yield
    0xCD, 0x20,                   // [44] INT 0x20

    // 7. Loop Back
    0xEB, 0xD0                    // [46] JMP -48 bytes (Back to index 0)
    // Size: 48 bytes.
};

int fat_compare_name(const char* input, char* fat_name, char* fat_ext) {
    // 1. HARD MATCH for "." and ".."
    // We check the first two bytes of fat_name directly
    if (input[0] == '.' && input[1] == '\0') {
        return (fat_name[0] == '.' && fat_name[1] == ' ');
    }
    if (input[0] == '.' && input[1] == '.' && input[2] == '\0') {
        return (fat_name[0] == '.' && fat_name[1] == '.');
    }

    // 2. Regular 8.3 Comparison (for everything else)
    char clean_name[13];
    // ... rest of your logic ...
    int p = 0;

    // Build Name part
    for (int i = 0; i < 8 && fat_name[i] != ' '; i++) {
        clean_name[p++] = fat_name[i];
    }
    
    // Build Extension part (if it exists)
    if (fat_ext[0] != ' ') {
        clean_name[p++] = '.';
        for (int i = 0; i < 3 && fat_ext[i] != ' '; i++) {
            clean_name[p++] = fat_ext[i];
        }
    }
    clean_name[p] = '\0';

    // 3. CASE-INSENSITIVE COMPARE
    // FAT usually stores names in UPPERCASE. 
    // If you type "cd lol", you need to match "LOL".
    return (kstrcasecmp(input, clean_name) == 0); 
}

uint32_t get_current_dir_lba() {
    // If we are at cluster 0, we must jump to the Root Directory start
    if (current_dir_cluster == 0) {
        return first_fat_sector + (bpb.num_fats * bpb.fat_size_16);
    }
    // Otherwise, calculate the LBA of the data cluster
    return cluster_to_lba(current_dir_cluster);
}
void fat_init() {
    uint8_t sector0[512];
    ide_read_sector(0, sector0);
    kmemcpy(&bpb, sector0, sizeof(struct fat_bpb));

    // Calculate locations
    root_dir_sectors = ((bpb.root_entry_count * 32) + (bpb.bytes_per_sector - 1)) / bpb.bytes_per_sector;
    first_fat_sector = bpb.reserved_sector_count;
    uint32_t first_root_dir_sector = first_fat_sector + (bpb.num_fats * bpb.fat_size_16);
    first_data_sector = first_root_dir_sector + root_dir_sectors;
header_t* tail = (header_t*)0xB00218; // The address of your B2
tail->size = (64 * 1024 * 1024) - ((uint32_t)tail - 0x800000) - sizeof(header_t); // 64MB
tail->is_free = 1;
tail->next = NULL;


}

// Helper to convert Cluster to LBA
uint32_t cluster_to_lba(uint32_t cluster) {
    return ((cluster - 2) * bpb.sectors_per_cluster) + first_data_sector;
}


void* fat_load_file(struct fat_dir_entry* entry) {
    if (entry->size == 0) return NULL;

    // 1. Allocate the full buffer (aligned to 512 for IDE safety)
    uint32_t alloc_size = ((entry->size + 511) / 512) * 512;
    uint8_t* buffer = (uint8_t*)kmalloc(alloc_size);
    if (!buffer) return NULL;

    uint16_t cluster = entry->first_cluster_low;
    uint32_t bytes_remaining = entry->size;
    uint32_t buffer_offset = 0;

    // 2. Follow the FAT Chain
    // 0xFFF8 to 0xFFFF are End of File markers
    while (cluster > 1 && cluster < 0xFFF8) {
        uint32_t lba = cluster_to_lba(cluster);
        
        // Read the entire cluster (usually 1 or more sectors)
        for (int i = 0; i < bpb.sectors_per_cluster; i++) {
            uint32_t to_read = (bytes_remaining > 512) ? 512 : bytes_remaining;
            
            // Temporary static buffer to keep the read safe from heap/stack noise
            static uint8_t tmp_sector[512]; 
            ide_read_sector(lba + i, tmp_sector);
            
            kmemcpy(buffer + buffer_offset, tmp_sector, to_read);
            
            buffer_offset += to_read;
            bytes_remaining -= to_read;
            
            if (bytes_remaining == 0) break;
        }

        if (bytes_remaining == 0) break;

        // 3. Look up the NEXT cluster in the FAT table
        cluster = fat_get_next_cluster(cluster);
    }

    return (void*)buffer;
}


void fat_cd(const char* path) {
    // 1. Use the Path Walker to find the cluster
    uint32_t target_cluster = fat_get_cluster_from_path(path);

    if (target_cluster != 0xFFFFFFFF) {
        // 2. Success: Update the global state
        current_dir_cluster = target_cluster;
        
        // 3. Optional: Feedback to the user
        kprintf_unsync("Moved to: ");
        fat_pwd(); // Use your recursive PWD to show where we are now
    } else {
        kprintf_unsync("CD: Could not find path '%s'\n", path);
    }
}

struct fat_dir_entry* fat_search(const char* filename) {
    uint8_t buffer[512];
    
    fat_disk_busy = 0; 

    uint32_t search_lba = get_current_dir_lba();
    uint32_t sectors_to_search = (current_dir_cluster == 0) ? root_dir_sectors : bpb.sectors_per_cluster;

    for (uint32_t s = 0; s < sectors_to_search; s++) {
        ide_read_sector(search_lba + s, buffer);
        struct fat_dir_entry* entries = (struct fat_dir_entry*)buffer;

        for (int i = 0; i < 16; i++) {
            if (entries[i].name[0] == 0x00) return NULL; // End of directory
            if ((unsigned char)entries[i].name[0] == 0xE5) continue; // Deleted
            if (entries[i].attr == 0x0F) continue; // Skip LFN junk

            if (fat_compare_name(filename, (char*)entries[i].name, (char*)entries[i].ext)) {
                
                // THREAD-SAFE FIX: Allocate memory for this specific task
                struct fat_dir_entry* out = (struct fat_dir_entry*)kmalloc(sizeof(struct fat_dir_entry));
                
                // Safety check in case we are out of memory
                if (out) {
                    kmemcpy(out, &entries[i], sizeof(struct fat_dir_entry));
                }
                return out;
            }
        }
    }
    return NULL;
}

void fat_ls() {
    uint8_t buffer[512];
    uint32_t dir_lba = get_current_dir_lba();
    ide_read_sector(dir_lba, buffer);

    struct fat_dir_entry* entry = (struct fat_dir_entry*)buffer;
    
    // Header in a neutral color (e.g., Gray or Yellow)
    kprintf_color(0xAAAAAA, "Type   Name             Size\n");
    kprintf_color(0xAAAAAA, "----------------------------\n");

    for (int i = 0; i < 16; i++) {
        if (entry[i].name[0] == 0x00) break;
        if ((unsigned char)entry[i].name[0] == 0xE5) continue;
        if (entry[i].attr == 0x0F) continue; 

        // 1. Determine Color and Type Prefix
        uint32_t color;
        if (entry[i].attr & 0x10) {
            color = 0x00FFFF; // Cyan for Directories
            kprintf_color(color, "[DIR]  ");
        } else {
            color = 0xFFFFFF; // White for Files
            kprintf_color(color, "       ");
        }

        // 2. Print Name (using the specific color)
        for (int n = 0; n < 8; n++) {
            if (entry[i].name[n] != ' ') {
char c = entry[i].name[n];
                // Assuming you have a kputc_color or similar, 
                // otherwise we use kprintf_color with %c
                if (c >= 'A' && c <= 'Z') c += 32;
                kprintf_color(color, "%c", c);
            }
        }

        // 3. Smart Conditional Dot
        int is_dot_entry = (entry[i].name[0] == '.');
        if (!is_dot_entry) {
            if (entry[i].ext[0] != ' ' || entry[i].ext[1] != ' ' || entry[i].ext[2] != ' ') {
                kprintf_color(color, ".");
                for (int e = 0; e < 3; e++) {
        if (entry[i].ext[e] != ' ') {
            char c = entry[i].ext[e];
            // ADD THIS: Convert to lowercase for display
            if (c >= 'A' && c <= 'Z') c += 32;
            kprintf_color(color, "%c", c);
        }
    }
            }
        }

        // 4. Print Size (back to neutral color to keep the focus on names)
        kprintf_color(0x888888, "  %d bytes\n", entry[i].size);
    }
    VESA_flip();
}


void fat_ls_cluster(uint32_t cluster) {
    uint8_t buffer[512];
    uint32_t dir_lba;

    // 1. Determine LBA based on the cluster passed in
    if (cluster == 0) {
        dir_lba = first_fat_sector + (bpb.num_fats * bpb.fat_size_16);
    } else {
        dir_lba = cluster_to_lba(cluster);
    }

    ide_read_sector(dir_lba, buffer);
    struct fat_dir_entry* entry = (struct fat_dir_entry*)buffer;
    
    //kprintf_color(0xAAAAAA, "Directory Listing (Cluster %d):\n", cluster);

    for (int i = 0; i < 16; i++) {
        if (entry[i].name[0] == 0x00) break;
        if ((unsigned char)entry[i].name[0] == 0xE5) continue;
        if (entry[i].attr == 0x0F) continue; // Skip LFN

        uint32_t entry_color;
        
        // 2. Set Color and Prefix based on Attribute
        if (entry[i].attr & 0x10) {
            entry_color = 0x00FFFF; // Cyan for Directories
            kprintf_color(entry_color, "- ");
        } else {
            entry_color = 0xFFFFFF; // White for Files
            kprintf_color(entry_color, "- ");
        }

        // 3. Print the name using the entry color
        for (int n = 0; n < 8; n++) {
            if (entry[i].name[n] != ' '){
char c = entry[i].name[n];
                if (c >= 'A' && c <= 'Z') c += 32; // Convert to lowercase
                kprintf_color(entry_color, "%c", c);
      } 
        }

        // 4. Dot and Extension logic (Skip dot for '.' and '..')
        if (entry[i].name[0] != '.') {
            if (entry[i].ext[0] != ' ' || entry[i].ext[1] != ' ' || entry[i].ext[2] != ' ') {
                kprintf_color(entry_color, ".");
                for (int e = 0; e < 3; e++) {
                    if (entry[i].ext[e] != ' ') {
                        char c = entry[i].ext[e];
                        if (c >= 'A' && c <= 'Z') c += 32; // Convert to lowercase
                        kprintf_color(entry_color, "%c", c);
                    }
                }
            }
        }

        // 5. Human-Readable Size Format (Integer Math Only)
        uint32_t sz = entry[i].size;
        uint32_t dim_color = 0x555555;
        
        // Add a visual spacer
        kprintf_color(dim_color, "  ");

        if (sz < 1024) {
            // Less than 1 KB: Show Bytes
            kprintf_color(dim_color, "%d B\n", sz);
        } 
        else if (sz < 1048576) { // 1024 * 1024
            // Show KB with 1 decimal place
            uint32_t kb = sz / 1024;
            uint32_t dec = ((sz % 1024) * 10) / 1024;
            kprintf_color(dim_color, "%d.%d KB\n", kb, dec);
        } 
        else if (sz < 1073741824) { // 1024 * 1024 * 1024
            // Show MB with 1 decimal place
            uint32_t mb = sz / 1048576;
            uint32_t dec = ((sz % 1048576) * 10) / 1048576;
            kprintf_color(dim_color, "%d.%d MB\n", mb, dec);
        } 
        else {
            // Show GB with 1 decimal place
            uint32_t gb = sz / 1073741824;
            uint32_t dec = ((sz % 1073741824) * 10) / 1073741824;
            kprintf_color(dim_color, "%d.%d GB\n", gb, dec);
        }
    }
    
    VESA_flip();
}uint32_t fat_get_current_cluster() {
    return current_dir_cluster;
}
void fat_print_fixed(const char* str, int len) {
    for (int i = 0; i < len; i++) {
        // FAT pads with spaces (0x20). 
        // We print the char as long as it isn't a space.
        if (str[i] != ' ') {
            // Use your low-level char print (likely VESA_draw_char or similar)
            kputc(str[i]); 
        }
    }
}
void fat_print_name_ext(unsigned char* name, unsigned char* ext) {
    // 1. Print the 8-character name
    for (int i = 0; i < 8; i++) {
        if (name[i] != ' ') kputc(name[i]);
    }

    // 2. ONLY print a dot if it's NOT "." or ".." AND has an extension
    if (name[0] != '.') {
        if (ext[0] != ' ' || ext[1] != ' ' || ext[2] != ' ') {
            kputc('.');
            for (int i = 0; i < 3; i++) {
                if (ext[i] != ' ') kputc(ext[i]);
            }
        }
    }
}
uint16_t fat_find_free_cluster() {
    uint8_t fat_buffer[512];
    // Search the FAT table (starts at first_fat_sector)
    for (uint32_t s = 0; s < bpb.fat_size_16; s++) {
        ide_read_sector(first_fat_sector + s, fat_buffer);
        uint16_t* entries = (uint16_t*)fat_buffer;
        
        for (int i = 0; i < 256; i++) {
            if (entries[i] == 0x0000) { // 0x0000 means free
                return (s * 256) + i;
            }
        }
    }
    return 0xFFFF; // Disk full
}

void fat_update_table(uint16_t cluster, uint16_t value) {
    uint8_t fat_buffer[512];
    uint32_t fat_offset = cluster * 2;
    uint32_t fat_sector = first_fat_sector + (fat_offset / 512);
    uint32_t ent_offset = fat_offset % 512;

    ide_read_sector(fat_sector, fat_buffer);
    *(uint16_t*)&fat_buffer[ent_offset] = value;
    ide_write_sector(fat_sector, fat_buffer); // You'll need ide_write_sector!
}

void ide_write_sector(uint32_t lba, uint8_t* buffer) {
    // 1. Wait for BSY to clear
    while (inb(0x1F7) & 0x80); 

    outb(0x1F6, 0xE0 | ((lba >> 24) & 0x0F));
    outb(0x1F2, 1);
    outb(0x1F3, (uint8_t)lba);
    outb(0x1F4, (uint8_t)(lba >> 8));
    outb(0x1F5, (uint8_t)(lba >> 16));
    outb(0x1F7, 0x30); // Write Command

    // 2. Wait for BSY to clear AND DRQ to set
    // This is where it usually hangs. We check for Errors too.
    while (1) {
        uint8_t status = inb(0x1F7);
        if (!(status & 0x80) && (status & 0x08)) break; // Not Busy and Data Ready
        if (status & 0x01) {
            kprintf_unsync("IDE Error during write!\n");
            return;
        }
    }

    // 3. Send data
    uint16_t* ptr = (uint16_t*)buffer;
    for (int i = 0; i < 256; i++) {
        outw(0x1F0, ptr[i]);
    }

    // 4. Flush the write (Important for some emulators)
    //outb(0x1F7, 0xE7); // Cache Flush command
    while (inb(0x1F7) & 0x80);
}

void fat_mkdir(const char* dirname) {
    // 1. ALLOCATION: We allocate 1024 bytes for a 512-byte need.
    // This creates a "No Man's Land" between buffers so an IDE overrun 
    // hits empty space instead of the next heap header.
    uint8_t* dir_buf = (uint8_t*)kmalloc(1024);
    uint8_t* new_dir_sector = (uint8_t*)kmalloc(1024);

    if (!dir_buf || !new_dir_sector) {
        kprintf_unsync("MKDIR Error: Heap collision or OOM\n");
        if (dir_buf) kfree(dir_buf);
        if (new_dir_sector) kfree(new_dir_sector);
        return;
    }

    // 2. Find a free cluster
    uint16_t new_cluster = fat_find_free_cluster();
    if (new_cluster == 0xFFFF) {
        kprintf_unsync("MKDIR Error: Disk Full\n");
        kfree(dir_buf);
        kfree(new_dir_sector);
        return;
    }

    // 3. Initialize the NEW directory cluster (DOT and DOTDOT)
    // We clear the buffer (using your 4-byte aligned kmemset logic)
    kmemset(new_dir_sector, 0, 512 / 4); 
    struct fat_dir_entry* dot_entries = (struct fat_dir_entry*)new_dir_sector;

    // Create "." (Self)
    kmemcpy(dot_entries[0].name, ".       ", 8);
    dot_entries[0].attr = 0x10;
    dot_entries[0].first_cluster_low = new_cluster;

    // Create ".." (Parent)
    kmemcpy(dot_entries[1].name, "..      ", 8);
    dot_entries[1].attr = 0x10;
    dot_entries[1].first_cluster_low = (uint16_t)current_dir_cluster;

    // Write new dir to disk
    ide_write_sector(cluster_to_lba(new_cluster), new_dir_sector);
    fat_update_table(new_cluster, 0xFFFF);

    // 4. Update the PARENT directory
    uint32_t parent_dir_lba = get_current_dir_lba();
    ide_read_sector(parent_dir_lba, dir_buf);
    struct fat_dir_entry* entries = (struct fat_dir_entry*)dir_buf;

    int slot = -1;
    for (int i = 0; i < 16; i++) {
        if (entries[i].name[0] == 0x00 || (unsigned char)entries[i].name[0] == 0xE5) {
            slot = i;
            break;
        }
    }

    if (slot != -1) {
        // Clear exactly one entry size (32 bytes / 4 = 8 longs)
        kmemset(&entries[slot], 0, sizeof(struct fat_dir_entry) / 4);
        
        // Format Name (Uppercase)
        for (int i = 0; i < 8; i++) entries[slot].name[i] = ' ';
        for (int i = 0; i < 3; i++) entries[slot].ext[i] = ' ';
        for (int i = 0; i < 8 && dirname[i] != '\0'; i++) {
            char c = dirname[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            entries[slot].name[i] = c;
        }

        entries[slot].attr = 0x10;
        entries[slot].first_cluster_low = new_cluster;
        entries[slot].size = 0;

        ide_write_sector(parent_dir_lba, dir_buf);
        kprintf_unsync("Directory '%s' created.\n", dirname);
    } else {
        kprintf_unsync("MKDIR Error: Parent dir full\n");
    }

    // 5. CLEANUP
    kfree(dir_buf);
    kfree(new_dir_sector);
}

void fat_touch(const char* filename) {
    uint8_t* dir_buf = (uint8_t*)kmalloc(512);
    if (!dir_buf) return;

    uint32_t dir_lba = get_current_dir_lba();
    ide_read_sector(dir_lba, dir_buf);
    struct fat_dir_entry* entries = (struct fat_dir_entry*)dir_buf;

    int slot = -1;
    for (int i = 0; i < 16; i++) {
        if (entries[i].name[0] == 0x00 || (unsigned char)entries[i].name[0] == 0xE5) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        kprintf_unsync("TOUCH Error: Directory full\n");
        kfree(dir_buf);
        return;
    }

    // FIX: Divide by 4 so it only clears exactly 32 bytes (8 DWORDS)
    kmemset(&entries[slot], 0, sizeof(struct fat_dir_entry) / 4);
    for (int i = 0; i < 8; i++) entries[slot].name[i] = ' ';
    for (int i = 0; i < 3; i++) entries[slot].ext[i] = ' ';

    int dot_pos = -1;
    for (int i = 0; filename[i] != '\0'; i++) {
        if (filename[i] == '.') { dot_pos = i; break; }
    }

    int name_len = (int)(dot_pos == -1) ? kstrlen(filename) : (size_t)dot_pos;
    for (int i = 0; i < name_len && i < 8; i++) {
        char c = filename[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        entries[slot].name[i] = c;
    }

    if (dot_pos != -1) {
        for (int i = 0; i < 3 && filename[dot_pos + 1 + i] != '\0'; i++) {
            char c = filename[dot_pos + 1 + i];
            if (c >= 'a' && c <= 'z') c -= 32;
            entries[slot].ext[i] = c;
        }
    }

    entries[slot].attr = 0x00; 
    entries[slot].first_cluster_low = 0; 
    entries[slot].size = 0;

    ide_write_sector(dir_lba, dir_buf);
    kprintf_unsync("Created file: %s\n", filename);

    kfree(dir_buf);
}void fat_hexdump_file(const char* filename) {
    // 1. Find the file
    struct fat_dir_entry* entry = fat_search(filename);
    
    if (!entry) {
        kprintf_unsync("HEXDUMP Error: File '%s' not found.\n", filename);
        return;
    }

    if (entry->attr & 0x10) {
        kprintf_unsync("HEXDUMP Error: '%s' is a directory.\n", filename);
        return;
    }

    if (entry->size == 0) {
        kprintf_unsync("File '%s' is empty (0 bytes).\n", filename);
        return;
    }

    // 2. Load the file into RAM
    void* data = fat_load_file(entry);
    
    if (data) {
        kprintf_unsync("Hexdump of %s (%d bytes):\n", filename, entry->size);
        
        // 3. Call your existing hexdump function
        hexdump(data, entry->size);
        
        // 4. Clean up memory!
        kfree(data);
    } else {
        kprintf_unsync("HEXDUMP Error: Failed to load file.\n");
    }
}


void fat_write_file(const char* filename, const char* data) {
    if (!filename || !data) return;

    uint32_t total_size = kstrlen(data);
    uint32_t dir_lba = get_current_dir_lba();
    
    // 1. Find the file in the directory
    ide_read_sector(dir_lba, global_fat_buf);
    struct fat_dir_entry* entries = (struct fat_dir_entry*)global_fat_buf;
    int slot = -1;
    for (int i = 0; i < 16; i++) {
        if (fat_compare_name(filename, (char*)entries[i].name, (char*)entries[i].ext)) {
            slot = i;
            break;
        }
    }
    if (slot == -1) return;

    // 2. Initial Cluster Setup
    uint16_t current_cluster = entries[slot].first_cluster_low;
    if (current_cluster == 0) {
        current_cluster = fat_find_free_cluster();
        if (current_cluster == 0xFFFF) return;
        fat_update_table(current_cluster, 0xFFFF);
        entries[slot].first_cluster_low = current_cluster;
    }

    // 3. Update Directory Entry Size immediately
    entries[slot].size = total_size;
    ide_write_sector(dir_lba, global_fat_buf);

    // 4. The Write Loop
    uint32_t bytes_written = 0;
    while (bytes_written < total_size) {
        // Clear local buffer and copy up to 512 bytes
        kmemset(raw_io_buffer, 0, 512 / 4);
        uint32_t chunk = (total_size - bytes_written > 512) ? 512 : (total_size - bytes_written);
        kmemcpy(raw_io_buffer, data + bytes_written, chunk);

        // Determine which sector within the cluster we are writing to
        uint32_t sector_in_cluster = (bytes_written / 512) % bpb.sectors_per_cluster;
        uint32_t data_lba = cluster_to_lba(current_cluster) + sector_in_cluster;
        
        ide_write_sector(data_lba, raw_io_buffer);
        bytes_written += chunk;

        // 5. Chain Expansion: If we still have data and just finished a cluster
        if (bytes_written < total_size && (bytes_written % (bpb.sectors_per_cluster * 512) == 0)) {
            uint16_t next = fat_get_next_cluster(current_cluster);
            
            // If the current chain ends but we need more space, grow it
            if (next >= 0xFFF8 || next == 0) { 
                next = fat_find_free_cluster();
                if (next == 0xFFFF) {
                    kprintf_unsync("Error: Disk Full\n");
                    return;
                }
                // LINK the current cluster to the new one
                fat_update_table(current_cluster, next);
                // Mark the NEW cluster as the End of Chain
                fat_update_table(next, 0xFFFF);
            }
            current_cluster = next;
        }
    }
    kprintf_unsync("Saved %d bytes to %s\n", total_size, filename);
}

void fat_rm(const char* filename) {
    uint8_t buffer[512];
    uint32_t dir_lba = get_current_dir_lba();
    ide_read_sector(dir_lba, buffer);
    struct fat_dir_entry* entries = (struct fat_dir_entry*)buffer;

    for (int i = 0; i < 16; i++) {
        if (fat_compare_name(filename, (char*)entries[i].name, (char*)entries[i].ext)) {
            if (entries[i].attr & 0x10) {
                kprintf_unsync("Error: %s is a directory. Use RMDIR.\n", filename);
                return;
            }

            // 1. Free the cluster chain in the FAT table
            uint16_t cluster = entries[i].first_cluster_low;
            while (cluster != 0 && cluster < 0xFFF8) {
                uint16_t next = fat_get_next_cluster(cluster);
                fat_update_table(cluster, 0x0000); // 0x0000 = Free
                cluster = next;
            }

            // 2. Mark the directory entry as deleted
            entries[i].name[0] = 0xE5; 
            ide_write_sector(dir_lba, buffer);
            
            kprintf_unsync("File '%s' removed.\n", filename);
            return;
        }
    }
    kprintf_unsync("Error: File not found.\n");
}
void fat_rmdir(const char* dirname) {
    if (kstrcmp(dirname, ".") == 0 || kstrcmp(dirname, "..") == 0) {
        kprintf_unsync("Error: Cannot remove . or ..\n");
        return;
    }

    uint8_t buffer[512];
    uint32_t dir_lba = get_current_dir_lba();
    ide_read_sector(dir_lba, buffer);
    struct fat_dir_entry* entries = (struct fat_dir_entry*)buffer;

    for (int i = 0; i < 16; i++) {
        if (fat_compare_name(dirname, (char*)entries[i].name, (char*)entries[i].ext)) {
            if (!(entries[i].attr & 0x10)) {
                kprintf_unsync("Error: %s is a file. Use RM.\n", dirname);
                return;
            }

            // 1. Free the directory's cluster
            uint16_t cluster = entries[i].first_cluster_low;
            if (cluster != 0) {
                fat_update_table(cluster, 0x0000);
            }

            // 2. Mark entry as deleted
            entries[i].name[0] = 0xE5;
            ide_write_sector(dir_lba, buffer);

            kprintf_unsync("Directory '%s' removed.\n", dirname);
            return;
        }
    }

    kprintf_unsync("Error: Directory not found.\n");
}
void fat_print_path_recursive(uint16_t cluster) {
    // Base Case: We reached the Root
    if (cluster == 0) {
        return;
    }

    // 1. Read the current cluster to find the ".." (parent) cluster
    uint8_t buf[512];
    ide_read_sector(cluster_to_lba(cluster), buf);
    struct fat_dir_entry* entries = (struct fat_dir_entry*)buf;

    // In subdirectories, entries[0] is "." and entries[1] is ".."
    uint16_t parent_cluster = entries[1].first_cluster_low;

    // 2. RECURSE: Go up to the parent first so we print from top-down
    fat_print_path_recursive(parent_cluster);

    // 3. After returning from the parent, find our name in that parent
    uint32_t parent_lba = (parent_cluster == 0) ? 
        (first_fat_sector + (bpb.num_fats * bpb.fat_size_16)) : 
        cluster_to_lba(parent_cluster);

    // Search parent directory for the entry pointing to our current 'cluster'
    ide_read_sector(parent_lba, buf);
    entries = (struct fat_dir_entry*)buf;

    kputc('/'); // Print separator
    for (int i = 0; i < 16; i++) {
        if (entries[i].first_cluster_low == cluster) {
            // Found this folder's entry! Print its name.
            for (int n = 0; n < 8 && entries[i].name[n] != ' '; n++) {
                char c = entries[i].name[n];
                
                // ADD THIS: Convert to lowercase for display
                if (c >= 'A' && c <= 'Z') c += 32; 
                
                kputc(c); // Print the lowercase character
            }
            return; 
        }
    }
}

void fat_pwd() {
    if (fat_get_current_cluster() == 0) {
        kprintf_unsync("/\n");
    } else {
        fat_print_path_recursive(fat_get_current_cluster());
        kprintf_unsync("\n");
    }
}
struct fat_dir_entry* fat_search_in(const char* filename, uint32_t start_cluster) {
    static struct fat_dir_entry result;
    uint8_t buffer[512];
    
    // Determine where to start searching
    uint32_t lba = (start_cluster == 0) ? 
        (first_fat_sector + (bpb.num_fats * bpb.fat_size_16)) : 
        cluster_to_lba(start_cluster);

    ide_read_sector(lba, buffer);
    struct fat_dir_entry* entries = (struct fat_dir_entry*)buffer;

    for (int i = 0; i < 16; i++) {
        if (entries[i].name[0] == 0x00) return NULL;
        if (fat_compare_name(filename, (char*)entries[i].name, (char*)entries[i].ext)) {
            kmemcpy(&result, &entries[i], sizeof(struct fat_dir_entry));
            return &result;
        }
    }
    return NULL;
}
uint32_t fat_get_cluster_from_path(const char* path) {
    // If path starts with '/', start at Root (0), otherwise start at Current
    uint32_t walk_cluster = (path[0] == '/') ? 0 : current_dir_cluster;
    
    char temp[128];
    kstrcpy(temp, path);
    char* part = temp;
    if (*part == '/') part++;

    char* next_part = part;
    while (next_part != NULL) {
        // Find next slash
        char* slash = NULL;
        for (char* c = next_part; *c != '\0'; c++) {
            if (*c == '/') {
                slash = c;
                break;
            }
        }

        if (slash) {
            *slash = '\0';
            struct fat_dir_entry* e = fat_search_in(next_part, walk_cluster);
            if (!e || !(e->attr & 0x10)) return 0xFFFFFFFF; // Error
            walk_cluster = e->first_cluster_low;
            next_part = slash + 1;
        } else {
            // Last part of path
            struct fat_dir_entry* e = fat_search_in(next_part, walk_cluster);
            if (!e || !(e->attr & 0x10)) return 0xFFFFFFFF;
            return e->first_cluster_low;
        }
    }
    return walk_cluster;
}

void ide_read_sector(uint32_t lba, uint8_t* buffer) {
    outb(IDE_PRIMARY_DRIVE_SEL, 0xE0 | ((lba >> 24) & 0x0F));
    outb(IDE_PRIMARY_SECCOUNT, 1);
    outb(IDE_PRIMARY_LBA_LOW, (uint8_t)lba);
    outb(IDE_PRIMARY_LBA_MID, (uint8_t)(lba >> 8));
    outb(IDE_PRIMARY_LBA_HIGH, (uint8_t)(lba >> 16));
    outb(IDE_PRIMARY_COMMAND, 0x20); // 0x20 is "Read Sectors"

    // Wait for the drive to be ready
    while (!(inb(IDE_PRIMARY_COMMAND) & 0x08));

    // Read 256 16-bit words (512 bytes total)
    uint16_t* ptr = (uint16_t*)buffer;
    for (int i = 0; i < 256; i++) {
        ptr[i] = inw(IDE_PRIMARY_DATA);
    }
}
uint16_t fat_get_next_cluster(uint16_t cluster) {
    static uint8_t fat_sector_buffer[512];
    uint32_t fat_offset = cluster * 2; // Each entry is 2 bytes
    uint32_t lba = bpb.reserved_sector_count + (fat_offset / 512);
    uint32_t entry_offset = fat_offset % 512;

    ide_read_sector(lba, fat_sector_buffer);
    return *(uint16_t*)&fat_sector_buffer[entry_offset];
}

void fat_write_file_raw(const char* filename, const unsigned char* data, size_t total_size) {
    if (!filename || !data || total_size == 0) return;

    // 1. Allocate Directory Buffer
    uint8_t* dir_buf = (uint8_t*)kmalloc(512);
    if (!dir_buf) {
        kprintf_unsync("Write Error: Could not allocate dir_buf\n");
        return;
    }

    uint32_t dir_lba = get_current_dir_lba();
    ide_read_sector(dir_lba, dir_buf);
    struct fat_dir_entry* entries = (struct fat_dir_entry*)dir_buf;
    
    int slot = -1;
    for (int i = 0; i < 16; i++) {
        if (fat_compare_name(filename, (char*)entries[i].name, (char*)entries[i].ext)) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        kfree(dir_buf);
        return;
    }

    // 2. Initial Cluster Setup
    uint16_t current_cluster = entries[slot].first_cluster_low;
    if (current_cluster < 2) { 
        current_cluster = fat_find_free_cluster();
        if (current_cluster == 0xFFFF) {
            kfree(dir_buf);
            return;
        }
        entries[slot].first_cluster_low = current_cluster;
        fat_update_table(current_cluster, 0xFFFF);
    }

    // 3. Update Size and Commit Directory Entry
    entries[slot].size = total_size;
    ide_write_sector(dir_lba, dir_buf);
    
    // We are done with the directory buffer, free it now to keep heap clean
    kfree(dir_buf);

    // 4. Allocate Sector Buffer for Data
    uint8_t* sector_scratch = (uint8_t*)kmalloc(512);
    if (!sector_scratch) {
        kprintf_unsync("Write Error: Could not allocate sector_scratch\n");
        return; 
    }

    uint32_t bytes_written = 0;
    while (bytes_written < total_size) {
        // Prepare the 512-byte chunk
        kmemset(sector_scratch, 0, 128); // 128 * 4 bytes
        uint32_t chunk = (total_size - bytes_written > 512) ? 512 : (total_size - bytes_written);
        kmemcpy(sector_scratch, data + bytes_written, chunk);

        // LBA Math
        uint32_t sector_in_cluster = (bytes_written / 512) % bpb.sectors_per_cluster;
        uint32_t data_lba = cluster_to_lba(current_cluster) + sector_in_cluster;
        
        ide_write_sector(data_lba, sector_scratch);
        bytes_written += chunk;

        // 5. Chain Expansion
        if (bytes_written < total_size && (bytes_written % (bpb.sectors_per_cluster * 512) == 0)) {
            uint16_t next = fat_get_next_cluster(current_cluster);
            
            if (next >= 0xFFF8 || next < 2) { 
                next = fat_find_free_cluster();
                if (next == 0xFFFF) {
                    kprintf_unsync("Error: Disk Full\n");
                    kfree(sector_scratch); // Free before emergency exit
                    return;
                }
                fat_update_table(current_cluster, next);
                fat_update_table(next, 0xFFFF);
            }
            current_cluster = next;
        }
    }

    // Final Cleanup
    kfree(sector_scratch);
    kprintf_unsync("Saved %d bytes to %s via Heap\n", total_size, filename);
}
void test_multi_sector_write() {
    uint32_t file_size = 2048; // Exactly 4 sectors
    uint8_t* test_buffer = (uint8_t*)kmalloc(file_size);

    if (!test_buffer) {
        kprintf_unsync("TEST Error: Could not allocate test buffer\n");
        return;
    }

    // 1. Fill the entire buffer with NOPs (0x90)
    kmemset(test_buffer, 0x90909090, file_size / 4);

    // 2. Place the Spinner code at the very beginning (Sector 0)
    // We copy 50 bytes (leaving out the original 2-byte short jump)
    kmemcpy(test_buffer, spinner_code, 50);

    // 3. Place a "Long Jump" at the very end (Sector 3)
    // This jump will point back to the beginning of the file (offset 0)
    // Opcode E9 is a relative jump with a 32-bit offset
    uint32_t jump_instruction_idx = file_size - 5; 
    test_buffer[jump_instruction_idx] = 0xE9;
    
    // Relative offset calculation: 
    // target_address - (current_instruction_address + 5)
    int32_t relative_offset = -(int32_t)(jump_instruction_idx + 5);
    kmemcpy(&test_buffer[jump_instruction_idx + 1], &relative_offset, 4);

    // 4. Create the file and write using the Heap-based function
    kprintf_unsync("Creating BIGSPIN.BIN (2048 bytes)...\n");
    fat_touch("BIGSPIN.BIN");
    fat_write_file_raw("BIGSPIN.BIN", test_buffer, file_size);

    // 5. Cleanup
    kfree(test_buffer);
    //kprintf_unsync("Test complete. Check 'ls' and run 'BIGSPIN.BIN'\n");
}
void fat_append_file(const char* filename, const char* data) {
    if (!filename || !data) return;

    struct fat_dir_entry* entry = fat_search(filename);
    if (!entry) {
        kprintf_unsync("Append Error: File not found.\n");
        return;
    }

    uint32_t append_len = kstrlen(data);
    uint32_t old_size = entry->size;
    uint32_t new_size = old_size + append_len;

    // 1. Find the LAST cluster of the existing file
    uint16_t current_cluster = entry->first_cluster_low;
    uint16_t last_cluster = current_cluster;

    if (current_cluster == 0) {
        // File is empty, just use normal write
        fat_write_file(filename, data);
        return;
    }

    while (1) {
        uint16_t next = fat_get_next_cluster(last_cluster);
        if (next >= 0xFFF8 || next < 2) break;
        last_cluster = next;
    }

    // 2. Handle the "Slack Space" in the last sector
    // Calculate how many bytes into the last cluster the current file ends
    uint32_t bytes_in_last_cluster = old_size % (bpb.sectors_per_cluster * 512);
    
    uint32_t data_offset = 0;

    if (bytes_in_last_cluster > 0) {
        // We need to read the last sector, patch it, and write it back
        uint32_t sector_in_cluster = (bytes_in_last_cluster / 512);
        uint32_t byte_offset_in_sector = bytes_in_last_cluster % 512;
        uint32_t slack_in_sector = 512 - byte_offset_in_sector;
        uint32_t lba = cluster_to_lba(last_cluster) + sector_in_cluster;

        uint8_t* scratch = (uint8_t*)kmalloc(512);
        ide_read_sector(lba, scratch);

        uint32_t to_write = (append_len < slack_in_sector) ? append_len : slack_in_sector;
        kmemcpy(scratch + byte_offset_in_sector, data, to_write);
        ide_write_sector(lba, scratch);
        kfree(scratch);

        data_offset += to_write;
    }

    // 3. If there's still data left, use the existing write loop logic to add clusters
    if (data_offset < append_len) {
        // We use a modified version of your write loop starting from the end of the chain
        uint32_t bytes_remaining = append_len - data_offset;
        const char* remaining_data = data + data_offset;
        uint32_t loop_written = 0;

        while (loop_written < bytes_remaining) {
            // Allocate new cluster if we just finished the current one
            // (Or if we were perfectly at the end of the last cluster)
            uint16_t next = fat_find_free_cluster();
            if (next == 0xFFFF) {
                kprintf_unsync("Disk Full during append!\n");
                break;
            }
            
            fat_update_table(last_cluster, next);
            fat_update_table(next, 0xFFFF);
            last_cluster = next;

            // Write up to a full cluster of data
            for (int s = 0; s < bpb.sectors_per_cluster && loop_written < bytes_remaining; s++) {
                uint8_t* sector_buf = (uint8_t*)kmalloc(512);
                kmemset(sector_buf, 0, 128);
                uint32_t chunk = (bytes_remaining - loop_written > 512) ? 512 : (bytes_remaining - loop_written);
                kmemcpy(sector_buf, remaining_data + loop_written, chunk);
                
                ide_write_sector(cluster_to_lba(last_cluster) + s, sector_buf);
                loop_written += chunk;
                kfree(sector_buf);
            }
        }
    }

    // 4. Final step: Update the directory entry with the NEW size
    // We search the directory again to get a fresh pointer to the buffer
    uint32_t dir_lba = get_current_dir_lba();
    uint8_t* dir_buf = (uint8_t*)kmalloc(512);
    ide_read_sector(dir_lba, dir_buf);
    struct fat_dir_entry* entries = (struct fat_dir_entry*)dir_buf;

    for (int i = 0; i < 16; i++) {
        if (fat_compare_name(filename, (char*)entries[i].name, (char*)entries[i].ext)) {
            entries[i].size = new_size;
            ide_write_sector(dir_lba, dir_buf);
            break;
        }
    }
    kfree(dir_buf);
}


// The VFS Write Wrapper
uint32_t fat_vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    // For now, we will treat VFS writes as complete file overwrites using your raw writer
    fat_write_file_raw(node->name, buffer, size);
    
    // Update the VFS node's size in memory so the OS knows it got bigger
    node->size = size; 
    
    return size; // Return the number of bytes written
}

// 1. The VFS Read Wrapper
uint32_t fat_vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    // We synthesize a fake directory entry so we can reuse your awesome fat_load_file!
    struct fat_dir_entry fake_entry;
    fake_entry.first_cluster_low = (uint16_t)(uint32_t)node->device_specific_data;
    fake_entry.size = node->size;

    void* raw_file = fat_load_file(&fake_entry);
    if (!raw_file) return 0; // Read failed

    // Safety checks
    if (offset >= node->size) {
        kfree(raw_file);
        return 0;
    }

    uint32_t real_size = size;
    if (offset + size > node->size) {
        real_size = node->size - offset; // Prevent reading out of bounds
    }

    // Copy the specific requested chunk into the VFS buffer
    kmemcpy(buffer, (uint8_t*)raw_file + offset, real_size);
    kfree(raw_file);

    return real_size;
}

// 2. The VFS Find Directory Wrapper
vfs_node_t* fat_vfs_finddir(vfs_node_t* node, char* name) {
    uint32_t search_cluster = (uint32_t)node->device_specific_data;
    struct fat_dir_entry* entry = fat_search_in(name, search_cluster);

    if (!entry) return NULL;

    vfs_node_t* out = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    kstrcpy(out->name, name);
    out->size = entry->size;
    
    // Attach the generic finddir so we can navigate subfolders
    out->finddir = fat_vfs_finddir; 
    
    if (entry->attr & 0x10) {
        out->flags = FS_DIRECTORY;
        out->read = 0;
        out->write = 0;
    } else {
        out->flags = FS_FILE;
        out->read = fat_vfs_read; 
        out->write = fat_vfs_write; // Correctly attach the write wrapper
    }
    
    out->device_specific_data = (void*)(uint32_t)entry->first_cluster_low;
    return out;
}
// 3. The Initial Mount Function
void fat_vfs_mount() {
    fs_root = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    kstrcpy(fs_root->name, "/");
    fs_root->flags = FS_DIRECTORY | FS_MOUNTPOINT;
    fs_root->size = 0;
    fs_root->read = 0;
    fs_root->write = 0;
    fs_root->finddir = fat_vfs_finddir; // Attach the finder!
    fs_root->device_specific_data = (void*)0; // Cluster 0 = Root dir
    fs_current_dir = fs_root; // NEW: Start in the root directory
}
