#ifndef FAT_H
#define FAT_H
#include <stdint.h>
#define IDE_PRIMARY_DATA       0x1F0
#define IDE_PRIMARY_ERR        0x1F1
#define IDE_PRIMARY_SECCOUNT   0x1F2
#define IDE_PRIMARY_LBA_LOW    0x1F3
#define IDE_PRIMARY_LBA_MID    0x1F4
#define IDE_PRIMARY_LBA_HIGH   0x1F5
#define IDE_PRIMARY_DRIVE_SEL  0x1F6
#define IDE_PRIMARY_COMMAND    0x1F7
struct fat_bpb {
    uint8_t  boot_jump[3];
    char     oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t  num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    // Extended Boot Record
    uint8_t  drive_number;
    uint8_t  reserved;
    uint8_t  boot_signature;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];
} __attribute__((packed));


struct fat_dir_entry {
    unsigned char name[8];
    unsigned char ext[3];
    uint8_t attr;
    uint8_t reserved;
    uint8_t create_time_ms;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t last_access_date;
    uint16_t first_cluster_high; // Always 0 in FAT16
    uint16_t last_write_time;
    uint16_t last_write_date;
    uint16_t first_cluster_low;  // The actual cluster
    uint32_t size;               // Offset 28
} __attribute__((packed));       

void fat_init();
uint32_t cluster_to_lba(uint32_t cluster); 
uint16_t fat_get_next_cluster(uint16_t cluster);
void* fat_load_file(struct fat_dir_entry* entry);
void fat_ls();

void ide_read_sector(uint32_t lba, uint8_t* buffer); 
int fat_compare_name(const char* input, char* fat_name, char* fat_ext); 
uint32_t get_current_dir_lba();
void fat_ls();
void fat_cd(const char* path);
uint32_t fat_get_current_cluster();
struct fat_dir_entry* fat_search(const char* filename);
void fat_print_fixed(const char* str, int len);
void fat_print_name_ext(unsigned char* name, unsigned char* ext);
void fat_update_table(uint16_t cluster, uint16_t value);
uint16_t fat_find_free_cluster();
void ide_write_sector(uint32_t lba, uint8_t* buffer);
void fat_mkdir(const char* dirname);
void fat_touch(const char* filename);
void fat_hexdump_file(const char* filename);
void fat_write_file(const char* filename, const char* data);
void fat_rm(const char* filename);
void fat_rmdir(const char* dirname);
void fat_pwd();
void fat_print_path_recursive(uint16_t cluster);
void fat_ls_cluster(uint32_t cluster);
struct fat_dir_entry* fat_search_in(const char* filename, uint32_t start_cluster);
uint32_t fat_get_cluster_from_path(const char* path);
void fat_write_file_raw(const char* filename, const uint8_t* data, uint32_t size);
void test_multi_sector_write();

void fat_append_file(const char* filename, const char* data);
#endif // !FAT_H
