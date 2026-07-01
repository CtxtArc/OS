#include "fat.h"
#include "io.h"
#include "kheap.h"
#include "lib.h"
#include "task.h"
#include "vesa.h"
#include "vfs.h"
#include <stdint.h>

struct fat_bpb bpb;
uint32_t root_dir_sectors;
uint32_t first_data_sector;
uint32_t first_fat_sector;

static uint32_t current_dir_cluster = 0; // 0 means Root Directory
static uint8_t global_fat_buf[512] __attribute__((aligned(16)));
static uint8_t raw_io_buffer[512] __attribute__((aligned(16)));
volatile int fat_disk_busy = 0;

unsigned char spinner_code[] = {
    0xB8, 0x02, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xC1, 0xE8, 0x05, 0x83,
    0xE0, 0x03, 0xBB, 0x2D, 0x5C, 0x7C, 0x2F, 0x88, 0xC1, 0xC1, 0xE1,
    0x03, 0xD3, 0xEB, 0x81, 0xE3, 0xFF, 0x00, 0x00, 0x00, 0xB8, 0x01,
    0x00, 0x00, 0x00, 0xB9, 0xE8, 0x03, 0x00, 0x00, 0xBA, 0x05, 0x00,
    0x00, 0x00, 0xCD, 0x80, 0xF4, 0x90, 0xEB, 0xCC};

unsigned char clock_code[] = {
    0xB8, 0x02, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xC1, 0xE8, 0x07, 0x83, 0xE0,
    0x0F, 0xBB, 0x0A, 0x00, 0x00, 0x00, 0x31, 0xD2, 0xF7, 0xF3, 0x83, 0xC2,
    0x30, 0x89, 0xD3, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xB9, 0xE8, 0x03, 0x00,
    0x00, 0xBA, 0x05, 0x00, 0x00, 0x00, 0xCD, 0x80, 0xCD, 0x20, 0xEB, 0xD0};

int fat_compare_name(const char *input, char *fat_name, char *fat_ext) {
  if (input[0] == '.' && input[1] == '\0') {
    return (fat_name[0] == '.' && fat_name[1] == ' ');
  }
  if (input[0] == '.' && input[1] == '.' && input[2] == '\0') {
    return (fat_name[0] == '.' && fat_name[1] == '.');
  }

  char clean_name[13];
  int p = 0;

  for (int i = 0; i < 8 && fat_name[i] != ' '; i++) {
    clean_name[p++] = fat_name[i];
  }

  if (fat_ext[0] != ' ') {
    clean_name[p++] = '.';
    for (int i = 0; i < 3 && fat_ext[i] != ' '; i++) {
      clean_name[p++] = fat_ext[i];
    }
  }
  clean_name[p] = '\0';

  return (kstrcasecmp(input, clean_name) == 0);
}

uint32_t get_current_dir_lba() {
  if (current_dir_cluster == 0) {
    return first_fat_sector + (bpb.num_fats * bpb.fat_size_16);
  }
  return cluster_to_lba(current_dir_cluster);
}

void fat_init() {
  uint8_t sector0[512];
  ide_read_sector(0, sector0);
  kmemcpy(&bpb, sector0, sizeof(struct fat_bpb));

  root_dir_sectors =
      ((bpb.root_entry_count * 32) + (bpb.bytes_per_sector - 1)) /
      bpb.bytes_per_sector;
  first_fat_sector = bpb.reserved_sector_count;
  uint32_t first_root_dir_sector =
      first_fat_sector + (bpb.num_fats * bpb.fat_size_16);
  first_data_sector = first_root_dir_sector + root_dir_sectors;

  header_t *tail = (header_t *)0xB00218;
  tail->size =
      (64 * 1024 * 1024) - ((uint32_t)tail - 0x800000) - sizeof(header_t);
  tail->is_free = 1;
  tail->next = NULL;
}

uint32_t cluster_to_lba(uint32_t cluster) {
  return ((cluster - 2) * bpb.sectors_per_cluster) + first_data_sector;
}

void *fat_load_file(struct fat_dir_entry *entry) {
  if (entry->size == 0)
    return NULL;

  uint32_t alloc_size = ((entry->size + 511) / 512) * 512;
  uint8_t *buffer = (uint8_t *)kmalloc(alloc_size);
  if (!buffer)
    return NULL;

  uint16_t cluster = entry->first_cluster_low;
  uint32_t bytes_remaining = entry->size;
  uint32_t buffer_offset = 0;

  while (cluster > 1 && cluster < 0xFFF8) {
    uint32_t lba = cluster_to_lba(cluster);

    for (int i = 0; i < bpb.sectors_per_cluster; i++) {
      uint32_t to_read = (bytes_remaining > 512) ? 512 : bytes_remaining;

      static uint8_t tmp_sector[512];
      ide_read_sector(lba + i, tmp_sector);

      kmemcpy(buffer + buffer_offset, tmp_sector, to_read);

      buffer_offset += to_read;
      bytes_remaining -= to_read;

      if (bytes_remaining == 0)
        break;
    }

    if (bytes_remaining == 0)
      break;

    cluster = fat_get_next_cluster(cluster);
  }

  return (void *)buffer;
}

void fat_cd(const char *path) {
  uint32_t target_cluster = fat_get_cluster_from_path(path);

  if (target_cluster != 0xFFFFFFFF) {
    current_dir_cluster = target_cluster;
    kprintf_unsync("Moved to: ");
    fat_pwd();
  } else {
    kprintf_unsync("CD: Could not find path '%s'\n", path);
  }
}

struct fat_dir_entry *fat_search(const char *filename) {
  uint8_t buffer[512];

  fat_disk_busy = 0;

  uint32_t search_lba = get_current_dir_lba();
  uint32_t sectors_to_search =
      (current_dir_cluster == 0) ? root_dir_sectors : bpb.sectors_per_cluster;

  for (uint32_t s = 0; s < sectors_to_search; s++) {
    ide_read_sector(search_lba + s, buffer);
    struct fat_dir_entry *entries = (struct fat_dir_entry *)buffer;

    for (int i = 0; i < 16; i++) {
      if (entries[i].name[0] == 0x00)
        return NULL;
      if ((unsigned char)entries[i].name[0] == 0xE5)
        continue;
      if (entries[i].attr == 0x0F)
        continue;

      if (fat_compare_name(filename, (char *)entries[i].name,
                           (char *)entries[i].ext)) {
        struct fat_dir_entry *out =
            (struct fat_dir_entry *)kmalloc(sizeof(struct fat_dir_entry));
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

  struct fat_dir_entry *entry = (struct fat_dir_entry *)buffer;

  kprintf_color(0xAAAAAA, "Type   Name             Size\n");
  kprintf_color(0xAAAAAA, "----------------------------\n");

  for (int i = 0; i < 16; i++) {
    if (entry[i].name[0] == 0x00)
      break;
    if ((unsigned char)entry[i].name[0] == 0xE5)
      continue;
    if (entry[i].attr == 0x0F)
      continue;

    uint32_t color;
    if (entry[i].attr & 0x10) {
      color = 0x00FFFF;
      kprintf_color(color, "[DIR]  ");
    } else {
      color = 0xFFFFFF;
      kprintf_color(color, "       ");
    }

    for (int n = 0; n < 8; n++) {
      if (entry[i].name[n] != ' ') {
        char c = entry[i].name[n];
        if (c >= 'A' && c <= 'Z')
          c += 32;
        kprintf_color(color, "%c", c);
      }
    }

    int is_dot_entry = (entry[i].name[0] == '.');
    if (!is_dot_entry) {
      if (entry[i].ext[0] != ' ' || entry[i].ext[1] != ' ' ||
          entry[i].ext[2] != ' ') {
        kprintf_color(color, ".");
        for (int e = 0; e < 3; e++) {
          if (entry[i].ext[e] != ' ') {
            char c = entry[i].ext[e];
            if (c >= 'A' && c <= 'Z')
              c += 32;
            kprintf_color(color, "%c", c);
          }
        }
      }
    }

    kprintf_color(0x888888, "  %d bytes\n", entry[i].size);
  }
  VESA_flip();
}

void fat_ls_cluster(uint32_t cluster) {
  uint8_t buffer[512];
  uint32_t dir_lba;

  if (cluster == 0) {
    dir_lba = first_fat_sector + (bpb.num_fats * bpb.fat_size_16);
  } else {
    dir_lba = cluster_to_lba(cluster);
  }

  ide_read_sector(dir_lba, buffer);
  struct fat_dir_entry *entry = (struct fat_dir_entry *)buffer;

  for (int i = 0; i < 16; i++) {
    if (entry[i].name[0] == 0x00)
      break;
    if ((unsigned char)entry[i].name[0] == 0xE5)
      continue;
    if (entry[i].attr == 0x0F)
      continue;

    uint32_t entry_color;

    if (entry[i].attr & 0x10) {
      entry_color = 0x00FFFF;
      kprintf_color(entry_color, "- ");
    } else {
      entry_color = 0xFFFFFF;
      kprintf_color(entry_color, "- ");
    }

    for (int n = 0; n < 8; n++) {
      if (entry[i].name[n] != ' ') {
        char c = entry[i].name[n];
        if (c >= 'A' && c <= 'Z')
          c += 32;
        kprintf_color(entry_color, "%c", c);
      }
    }

    if (entry[i].name[0] != '.') {
      if (entry[i].ext[0] != ' ' || entry[i].ext[1] != ' ' ||
          entry[i].ext[2] != ' ') {
        kprintf_color(entry_color, ".");
        for (int e = 0; e < 3; e++) {
          if (entry[i].ext[e] != ' ') {
            char c = entry[i].ext[e];
            if (c >= 'A' && c <= 'Z')
              c += 32;
            kprintf_color(entry_color, "%c", c);
          }
        }
      }
    }

    uint32_t sz = entry[i].size;
    uint32_t dim_color = 0x555555;

    kprintf_color(dim_color, "  ");

    if (sz < 1024) {
      kprintf_color(dim_color, "%d B\n", sz);
    } else if (sz < 1048576) {
      uint32_t kb = sz / 1024;
      uint32_t dec = ((sz % 1024) * 10) / 1024;
      kprintf_color(dim_color, "%d.%d KB\n", kb, dec);
    } else if (sz < 1073741824) {
      uint32_t mb = sz / 1048576;
      uint32_t dec = ((sz % 1048576) * 10) / 1048576;
      kprintf_color(dim_color, "%d.%d MB\n", mb, dec);
    } else {
      uint32_t gb = sz / 1073741824;
      uint32_t dec = ((sz % 1073741824) * 10) / 1073741824;
      kprintf_color(dim_color, "%d.%d GB\n", gb, dec);
    }
  }

  VESA_flip();
}

uint32_t fat_get_current_cluster() { return current_dir_cluster; }

void fat_print_fixed(const char *str, int len) {
  for (int i = 0; i < len; i++) {
    if (str[i] != ' ') {
      kputc(str[i]);
    }
  }
}

void fat_print_name_ext(unsigned char *name, unsigned char *ext) {
  for (int i = 0; i < 8; i++) {
    if (name[i] != ' ')
      kputc(name[i]);
  }

  if (name[0] != '.') {
    if (ext[0] != ' ' || ext[1] != ' ' || ext[2] != ' ') {
      kputc('.');
      for (int i = 0; i < 3; i++) {
        if (ext[i] != ' ')
          kputc(ext[i]);
      }
    }
  }
}

static uint16_t free_cluster_hint_sector = 0;

uint16_t fat_find_free_cluster() {
  uint8_t fat_buffer[512];
  for (uint32_t pass = 0; pass < bpb.fat_size_16; pass++) {
    uint32_t s = (free_cluster_hint_sector + pass) % bpb.fat_size_16;
    ide_read_sector(first_fat_sector + s, fat_buffer);
    uint16_t *entries = (uint16_t *)fat_buffer;

    for (int i = 0; i < 256; i++) {
      if (entries[i] == 0x0000) {
        free_cluster_hint_sector = s;
        return (s * 256) + i;
      }
    }
  }
  return 0xFFFF;
}

void fat_update_table(uint16_t cluster, uint16_t value) {
  uint8_t fat_buffer[512];
  uint32_t fat_offset = cluster * 2;
  uint32_t fat_sector = first_fat_sector + (fat_offset / 512);
  uint32_t ent_offset = fat_offset % 512;

  ide_read_sector(fat_sector, fat_buffer);
  *(uint16_t *)&fat_buffer[ent_offset] = value;
  ide_write_sector(fat_sector, fat_buffer);
}

void ide_write_sector(uint32_t lba, uint8_t *buffer) {
  uint32_t spins = 0;
  while (inb(0x1F7) & 0x80) {
    if (++spins > 100000) { // safety valve only, not the normal path
      kprintf_unsync("IDE Error: read timeout\n");
      return;
    }
  }

  outb(0x1F6, 0xE0 | ((lba >> 24) & 0x0F));
  outb(0x1F2, 1);
  outb(0x1F3, (uint8_t)lba);
  outb(0x1F4, (uint8_t)(lba >> 8));
  outb(0x1F5, (uint8_t)(lba >> 16));
  outb(0x1F7, 0x30);

  while (1) {
    uint8_t status = inb(0x1F7);
    if (!(status & 0x80) && (status & 0x08))
      break;
    if (status & 0x01) {
      kprintf_unsync("IDE Error during write!\n");
      return;
    }
    yield();
  }

  uint16_t *ptr = (uint16_t *)buffer;
  for (int i = 0; i < 256; i++) {
    outw(0x1F0, ptr[i]);
  }
  spins = 0;
  while (inb(0x1F7) & 0x80) {
    if (++spins > 100000) { // safety valve only, not the normal path
      kprintf_unsync("IDE Error: read timeout\n");
      return;
    }
  }
}

void fat_mkdir(const char *dirname) {
  uint8_t *dir_buf = (uint8_t *)kmalloc(1024);
  uint8_t *new_dir_sector = (uint8_t *)kmalloc(1024);

  if (!dir_buf || !new_dir_sector) {
    kprintf_unsync("MKDIR Error: Heap collision or OOM\n");
    if (dir_buf)
      kfree(dir_buf);
    if (new_dir_sector)
      kfree(new_dir_sector);
    return;
  }

  uint16_t new_cluster = fat_find_free_cluster();
  if (new_cluster == 0xFFFF) {
    kprintf_unsync("MKDIR Error: Disk Full\n");
    kfree(dir_buf);
    kfree(new_dir_sector);
    return;
  }

  kmemset(new_dir_sector, 0, 512 / 4);
  struct fat_dir_entry *dot_entries = (struct fat_dir_entry *)new_dir_sector;

  kmemcpy(dot_entries[0].name, ".       ", 8);
  dot_entries[0].attr = 0x10;
  dot_entries[0].first_cluster_low = new_cluster;

  kmemcpy(dot_entries[1].name, "..      ", 8);
  dot_entries[1].attr = 0x10;
  dot_entries[1].first_cluster_low = (uint16_t)current_dir_cluster;

  ide_write_sector(cluster_to_lba(new_cluster), new_dir_sector);
  fat_update_table(new_cluster, 0xFFFF);

  uint32_t parent_dir_lba = get_current_dir_lba();
  ide_read_sector(parent_dir_lba, dir_buf);
  struct fat_dir_entry *entries = (struct fat_dir_entry *)dir_buf;

  int slot = -1;
  for (int i = 0; i < 16; i++) {
    if (entries[i].name[0] == 0x00 ||
        (unsigned char)entries[i].name[0] == 0xE5) {
      slot = i;
      break;
    }
  }

  if (slot != -1) {
    kmemset(&entries[slot], 0, sizeof(struct fat_dir_entry) / 4);

    for (int i = 0; i < 8; i++)
      entries[slot].name[i] = ' ';
    for (int i = 0; i < 3; i++)
      entries[slot].ext[i] = ' ';
    for (int i = 0; i < 8 && dirname[i] != '\0'; i++) {
      char c = dirname[i];
      if (c >= 'a' && c <= 'z')
        c -= 32;
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

  kfree(dir_buf);
  kfree(new_dir_sector);
}

void fat_touch(const char *filename) {
  uint8_t *dir_buf = (uint8_t *)kmalloc(512);
  if (!dir_buf)
    return;

  uint32_t dir_lba = get_current_dir_lba();
  ide_read_sector(dir_lba, dir_buf);
  struct fat_dir_entry *entries = (struct fat_dir_entry *)dir_buf;

  int slot = -1;
  for (int i = 0; i < 16; i++) {
    if (entries[i].name[0] == 0x00 ||
        (unsigned char)entries[i].name[0] == 0xE5) {
      slot = i;
      break;
    }
  }

  if (slot == -1) {
    kprintf_unsync("TOUCH Error: Directory full\n");
    kfree(dir_buf);
    return;
  }

  kmemset(&entries[slot], 0, sizeof(struct fat_dir_entry) / 4);
  for (int i = 0; i < 8; i++)
    entries[slot].name[i] = ' ';
  for (int i = 0; i < 3; i++)
    entries[slot].ext[i] = ' ';

  int dot_pos = -1;
  for (int i = 0; filename[i] != '\0'; i++) {
    if (filename[i] == '.') {
      dot_pos = i;
      break;
    }
  }

  int name_len = (int)(dot_pos == -1) ? kstrlen(filename) : (size_t)dot_pos;
  for (int i = 0; i < name_len && i < 8; i++) {
    char c = filename[i];
    if (c >= 'a' && c <= 'z')
      c -= 32;
    entries[slot].name[i] = c;
  }

  if (dot_pos != -1) {
    for (int i = 0; i < 3 && filename[dot_pos + 1 + i] != '\0'; i++) {
      char c = filename[dot_pos + 1 + i];
      if (c >= 'a' && c <= 'z')
        c -= 32;
      entries[slot].ext[i] = c;
    }
  }

  entries[slot].attr = 0x00;
  entries[slot].first_cluster_low = 0;
  entries[slot].size = 0;

  ide_write_sector(dir_lba, dir_buf);
  kprintf_unsync("Created file: %s\n", filename);

  kfree(dir_buf);
}

void fat_hexdump_file(const char *filename) {
  struct fat_dir_entry *entry = fat_search(filename);

  if (!entry) {
    kprintf_unsync("HEXDUMP Error: File '%s' not found.\n", filename);
    return;
  }

  if (entry->attr & 0x10) {
    kprintf_unsync("HEXDUMP Error: '%s' is a directory.\n", filename);
    kfree(entry);
    return;
  }

  if (entry->size == 0) {
    kprintf_unsync("File '%s' is empty (0 bytes).\n", filename);
    kfree(entry);
    return;
  }

  void *data = fat_load_file(entry);

  if (data) {
    kprintf_unsync("Hexdump of %s (%d bytes):\n", filename, entry->size);
    hexdump(data, entry->size);
    kfree(data);
  } else {
    kprintf_unsync("HEXDUMP Error: Failed to load file.\n");
  }
  kfree(entry);
}

void fat_write_file(const char *filename, const char *data) {
  if (!filename || !data)
    return;

  uint32_t total_size = kstrlen(data);
  uint32_t dir_lba = get_current_dir_lba();

  ide_read_sector(dir_lba, global_fat_buf);
  struct fat_dir_entry *entries = (struct fat_dir_entry *)global_fat_buf;
  int slot = -1;
  for (int i = 0; i < 16; i++) {
    if (fat_compare_name(filename, (char *)entries[i].name,
                         (char *)entries[i].ext)) {
      slot = i;
      break;
    }
  }
  if (slot == -1)
    return;

  uint16_t current_cluster = entries[slot].first_cluster_low;
  if (current_cluster == 0) {
    current_cluster = fat_find_free_cluster();
    if (current_cluster == 0xFFFF)
      return;
    fat_update_table(current_cluster, 0xFFFF);
    entries[slot].first_cluster_low = current_cluster;
  }

  entries[slot].size = total_size;
  ide_write_sector(dir_lba, global_fat_buf);

  uint32_t bytes_written = 0;
  while (bytes_written < total_size) {
    kmemset(raw_io_buffer, 0, 512 / 4);
    uint32_t chunk =
        (total_size - bytes_written > 512) ? 512 : (total_size - bytes_written);
    kmemcpy(raw_io_buffer, data + bytes_written, chunk);

    uint32_t sector_in_cluster =
        (bytes_written / 512) % bpb.sectors_per_cluster;
    uint32_t data_lba = cluster_to_lba(current_cluster) + sector_in_cluster;

    ide_write_sector(data_lba, raw_io_buffer);
    bytes_written += chunk;

    if (bytes_written < total_size &&
        (bytes_written % (bpb.sectors_per_cluster * 512) == 0)) {
      uint16_t next = fat_get_next_cluster(current_cluster);

      if (next >= 0xFFF8 || next == 0) {
        next = fat_find_free_cluster();
        if (next == 0xFFFF) {
          kprintf_unsync("Error: Disk Full\n");
          return;
        }
        fat_update_table(current_cluster, next);
        fat_update_table(next, 0xFFFF);
      }
      current_cluster = next;
    }
  }
  kprintf_unsync("Saved %d bytes to %s\n", total_size, filename);
}

void fat_rm(const char *filename) {
  uint8_t buffer[512];
  uint32_t dir_lba = get_current_dir_lba();
  ide_read_sector(dir_lba, buffer);
  struct fat_dir_entry *entries = (struct fat_dir_entry *)buffer;

  for (int i = 0; i < 16; i++) {
    if (fat_compare_name(filename, (char *)entries[i].name,
                         (char *)entries[i].ext)) {
      if (entries[i].attr & 0x10) {
        kprintf_unsync("Error: %s is a directory. Use RMDIR.\n", filename);
        return;
      }

      uint16_t cluster = entries[i].first_cluster_low;
      while (cluster != 0 && cluster < 0xFFF8) {
        uint16_t next = fat_get_next_cluster(cluster);
        fat_update_table(cluster, 0x0000);
        cluster = next;
      }

      entries[i].name[0] = 0xE5;
      ide_write_sector(dir_lba, buffer);

      kprintf_unsync("File '%s' removed.\n", filename);
      return;
    }
  }
  kprintf_unsync("Error: File not found.\n");
}

void fat_rmdir(const char *dirname) {
  if (kstrcmp(dirname, ".") == 0 || kstrcmp(dirname, "..") == 0) {
    kprintf_unsync("Error: Cannot remove . or ..\n");
    return;
  }

  uint8_t buffer[512];
  uint32_t dir_lba = get_current_dir_lba();
  ide_read_sector(dir_lba, buffer);
  struct fat_dir_entry *entries = (struct fat_dir_entry *)buffer;

  for (int i = 0; i < 16; i++) {
    if (fat_compare_name(dirname, (char *)entries[i].name,
                         (char *)entries[i].ext)) {
      if (!(entries[i].attr & 0x10)) {
        kprintf_unsync("Error: %s is a file. Use RM.\n", dirname);
        return;
      }

      uint16_t cluster = entries[i].first_cluster_low;
      if (cluster != 0) {
        fat_update_table(cluster, 0x0000);
      }

      entries[i].name[0] = 0xE5;
      ide_write_sector(dir_lba, buffer);

      kprintf_unsync("Directory '%s' removed.\n", dirname);
      return;
    }
  }

  kprintf_unsync("Error: Directory not found.\n");
}

void fat_print_path_recursive(uint16_t cluster) {
  if (cluster == 0) {
    return;
  }

  uint8_t buf[512];
  ide_read_sector(cluster_to_lba(cluster), buf);
  struct fat_dir_entry *entries = (struct fat_dir_entry *)buf;

  uint16_t parent_cluster = entries[1].first_cluster_low;

  fat_print_path_recursive(parent_cluster);

  uint32_t parent_lba =
      (parent_cluster == 0)
          ? (first_fat_sector + (bpb.num_fats * bpb.fat_size_16))
          : cluster_to_lba(parent_cluster);

  ide_read_sector(parent_lba, buf);
  entries = (struct fat_dir_entry *)buf;

  kputc('/');
  for (int i = 0; i < 16; i++) {
    if (entries[i].first_cluster_low == cluster) {
      for (int n = 0; n < 8 && entries[i].name[n] != ' '; n++) {
        char c = entries[i].name[n];
        if (c >= 'A' && c <= 'Z')
          c += 32;
        kputc(c);
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

struct fat_dir_entry *fat_search_in(const char *filename,
                                    uint32_t start_cluster) {
  static struct fat_dir_entry result;
  uint8_t buffer[512];

  uint32_t lba = (start_cluster == 0)
                     ? (first_fat_sector + (bpb.num_fats * bpb.fat_size_16))
                     : cluster_to_lba(start_cluster);

  ide_read_sector(lba, buffer);
  struct fat_dir_entry *entries = (struct fat_dir_entry *)buffer;

  for (int i = 0; i < 16; i++) {
    if (entries[i].name[0] == 0x00)
      return NULL;
    if (fat_compare_name(filename, (char *)entries[i].name,
                         (char *)entries[i].ext)) {
      kmemcpy(&result, &entries[i], sizeof(struct fat_dir_entry));
      return &result;
    }
  }
  return NULL;
}

uint32_t fat_get_cluster_from_path(const char *path) {
  uint32_t walk_cluster = (path[0] == '/') ? 0 : current_dir_cluster;

  char temp[128];
  kstrcpy(temp, path);
  char *part = temp;
  if (*part == '/')
    part++;

  char *next_part = part;
  while (next_part != NULL) {
    char *slash = NULL;
    for (char *c = next_part; *c != '\0'; c++) {
      if (*c == '/') {
        slash = c;
        break;
      }
    }

    if (slash) {
      *slash = '\0';
      struct fat_dir_entry *e = fat_search_in(next_part, walk_cluster);
      if (!e || !(e->attr & 0x10))
        return 0xFFFFFFFF;
      walk_cluster = e->first_cluster_low;
      next_part = slash + 1;
    } else {
      struct fat_dir_entry *e = fat_search_in(next_part, walk_cluster);
      if (!e || !(e->attr & 0x10))
        return 0xFFFFFFFF;
      return e->first_cluster_low;
    }
  }
  return walk_cluster;
}

void ide_read_sector(uint32_t lba, uint8_t *buffer) {
  outb(IDE_PRIMARY_DRIVE_SEL, 0xE0 | ((lba >> 24) & 0x0F));
  outb(IDE_PRIMARY_SECCOUNT, 1);
  outb(IDE_PRIMARY_LBA_LOW, (uint8_t)lba);
  outb(IDE_PRIMARY_LBA_MID, (uint8_t)(lba >> 8));
  outb(IDE_PRIMARY_LBA_HIGH, (uint8_t)(lba >> 16));
  outb(IDE_PRIMARY_COMMAND, 0x20);

  uint32_t spins = 0;
  while (!(inb(IDE_PRIMARY_COMMAND) & 0x08)) {
    if (++spins > 100000) { // safety valve only, not the normal path
      kprintf_unsync("IDE Error: read timeout\n");
      return;
    }
  }

  uint16_t *ptr = (uint16_t *)buffer;
  for (int i = 0; i < 256; i++) {
    ptr[i] = inw(IDE_PRIMARY_DATA);
  }
}

uint16_t fat_get_next_cluster(uint16_t cluster) {
  static uint8_t fat_sector_buffer[512];
  uint32_t fat_offset = cluster * 2;
  uint32_t lba = bpb.reserved_sector_count + (fat_offset / 512);
  uint32_t entry_offset = fat_offset % 512;

  ide_read_sector(lba, fat_sector_buffer);
  return *(uint16_t *)&fat_sector_buffer[entry_offset];
}

void fat_write_file_raw(const char *filename, const unsigned char *data,
                        size_t total_size) {
  if (!filename || !data || total_size == 0)
    return;

  uint8_t *dir_buf = (uint8_t *)kmalloc(512);
  if (!dir_buf) {
    kprintf_unsync("Write Error: Could not allocate dir_buf\n");
    return;
  }

  uint32_t dir_lba = get_current_dir_lba();
  ide_read_sector(dir_lba, dir_buf);
  struct fat_dir_entry *entries = (struct fat_dir_entry *)dir_buf;

  int slot = -1;
  for (int i = 0; i < 16; i++) {
    if (fat_compare_name(filename, (char *)entries[i].name,
                         (char *)entries[i].ext)) {
      slot = i;
      break;
    }
  }

  if (slot == -1) {
    kfree(dir_buf);
    return;
  }

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

  entries[slot].size = total_size;
  ide_write_sector(dir_lba, dir_buf);

  kfree(dir_buf);

  uint8_t *sector_scratch = (uint8_t *)kmalloc(512);
  if (!sector_scratch) {
    kprintf_unsync("Write Error: Could not allocate sector_scratch\n");
    return;
  }

  uint32_t bytes_written = 0;
  while (bytes_written < total_size) {
    kmemset(sector_scratch, 0, 128);
    uint32_t chunk =
        (total_size - bytes_written > 512) ? 512 : (total_size - bytes_written);
    kmemcpy(sector_scratch, data + bytes_written, chunk);

    uint32_t sector_in_cluster =
        (bytes_written / 512) % bpb.sectors_per_cluster;
    uint32_t data_lba = cluster_to_lba(current_cluster) + sector_in_cluster;

    ide_write_sector(data_lba, sector_scratch);
    bytes_written += chunk;

    if (bytes_written < total_size &&
        (bytes_written % (bpb.sectors_per_cluster * 512) == 0)) {
      uint16_t next = fat_get_next_cluster(current_cluster);

      if (next >= 0xFFF8 || next < 2) {
        next = fat_find_free_cluster();
        if (next == 0xFFFF) {
          kprintf_unsync("Error: Disk Full\n");
          kfree(sector_scratch);
          return;
        }
        fat_update_table(current_cluster, next);
        fat_update_table(next, 0xFFFF);
      }
      current_cluster = next;
    }
  }

  kfree(sector_scratch);
  kprintf_unsync("Saved %d bytes to %s via Heap\n", total_size, filename);
}

void test_multi_sector_write() {
  uint32_t file_size = 2048;
  uint8_t *test_buffer = (uint8_t *)kmalloc(file_size);

  if (!test_buffer) {
    kprintf_unsync("TEST Error: Could not allocate test buffer\n");
    return;
  }

  kmemset(test_buffer, 0x90909090, file_size / 4);
  kmemcpy(test_buffer, spinner_code, 50);

  uint32_t jump_instruction_idx = file_size - 5;
  test_buffer[jump_instruction_idx] = 0xE9;

  int32_t relative_offset = -(int32_t)(jump_instruction_idx + 5);
  kmemcpy(&test_buffer[jump_instruction_idx + 1], &relative_offset, 4);

  kprintf_unsync("Creating BIGSPIN.BIN (2048 bytes)...\n");
  fat_touch("BIGSPIN.BIN");
  fat_write_file_raw("BIGSPIN.BIN", test_buffer, file_size);

  kfree(test_buffer);
}

void fat_append_file(const char *filename, const char *data) {
  if (!filename || !data)
    return;

  struct fat_dir_entry *entry = fat_search(filename);
  if (!entry) {
    kprintf_unsync("Append Error: File not found.\n");
    return;
  }

  uint32_t append_len = kstrlen(data);
  uint32_t old_size = entry->size;
  uint32_t new_size = old_size + append_len;

  uint16_t current_cluster = entry->first_cluster_low;
  uint16_t last_cluster = current_cluster;

  if (current_cluster == 0) {
    kfree(entry);
    fat_write_file(filename, data);
    return;
  }
  kfree(entry); // CRITICAL: fat_search() heap-allocates; free it here, we
                // already copied the fields we need above.

  while (1) {
    uint16_t next = fat_get_next_cluster(last_cluster);
    if (next >= 0xFFF8 || next < 2)
      break;
    last_cluster = next;
  }

  uint32_t bytes_in_last_cluster = old_size % (bpb.sectors_per_cluster * 512);

  uint32_t data_offset = 0;

  if (bytes_in_last_cluster > 0) {
    uint32_t sector_in_cluster = (bytes_in_last_cluster / 512);
    uint32_t byte_offset_in_sector = bytes_in_last_cluster % 512;
    uint32_t slack_in_sector = 512 - byte_offset_in_sector;
    uint32_t lba = cluster_to_lba(last_cluster) + sector_in_cluster;

    uint8_t *scratch = (uint8_t *)kmalloc(512);
    ide_read_sector(lba, scratch);

    uint32_t to_write =
        (append_len < slack_in_sector) ? append_len : slack_in_sector;
    kmemcpy(scratch + byte_offset_in_sector, data, to_write);
    ide_write_sector(lba, scratch);
    kfree(scratch);

    data_offset += to_write;
  }

  if (data_offset < append_len) {
    uint32_t bytes_remaining = append_len - data_offset;
    const char *remaining_data = data + data_offset;
    uint32_t loop_written = 0;

    while (loop_written < bytes_remaining) {
      uint16_t next = fat_find_free_cluster();
      if (next == 0xFFFF) {
        kprintf_unsync("Disk Full during append!\n");
        break;
      }

      fat_update_table(last_cluster, next);
      fat_update_table(next, 0xFFFF);
      last_cluster = next;

      for (int s = 0;
           s < bpb.sectors_per_cluster && loop_written < bytes_remaining; s++) {
        uint8_t *sector_buf = (uint8_t *)kmalloc(512);
        kmemset(sector_buf, 0, 128);
        uint32_t chunk = (bytes_remaining - loop_written > 512)
                             ? 512
                             : (bytes_remaining - loop_written);
        kmemcpy(sector_buf, remaining_data + loop_written, chunk);

        ide_write_sector(cluster_to_lba(last_cluster) + s, sector_buf);
        loop_written += chunk;
        kfree(sector_buf);
      }
    }
  }

  uint32_t dir_lba = get_current_dir_lba();
  uint8_t *dir_buf = (uint8_t *)kmalloc(512);
  ide_read_sector(dir_lba, dir_buf);
  struct fat_dir_entry *entries = (struct fat_dir_entry *)dir_buf;

  for (int i = 0; i < 16; i++) {
    if (fat_compare_name(filename, (char *)entries[i].name,
                         (char *)entries[i].ext)) {
      entries[i].size = new_size;
      ide_write_sector(dir_lba, dir_buf);
      break;
    }
  }
  kfree(dir_buf);
}

// =============================================================================
// VFS ADAPTER — appended below the original driver.
//
// fat_vfs_* are static: they are only ever reached through vfs_node_t function
// pointers (read/write/finddir/readdir/create/unlink/mkdir/rmdir), never
// called by name from outside this file. The only public symbol from this
// section is fat_vfs_mount(), declared in fat.h.
// =============================================================================

static vfs_node_t *fat_vfs_finddir(vfs_node_t *, char *);
static int fat_vfs_readdir(vfs_node_t *, uint32_t, vfs_node_t *);
static int fat_vfs_create(vfs_node_t *, char *, uint32_t);
static int fat_vfs_unlink(vfs_node_t *, char *);
static int fat_vfs_mkdir_vfs(vfs_node_t *, char *);
static int fat_vfs_rmdir_vfs(vfs_node_t *, char *);
static uint32_t fat_vfs_read(vfs_node_t *, uint32_t, uint32_t, uint8_t *);
static uint32_t fat_vfs_write(vfs_node_t *, uint32_t, uint32_t, uint8_t *);

static uint32_t fat_dir_lba(uint32_t cluster) {
  if (cluster == 0)
    return first_fat_sector + (bpb.num_fats * bpb.fat_size_16);
  return cluster_to_lba(cluster);
}

static uint32_t fat_dir_sectors(uint32_t cluster) {
  return (cluster == 0) ? root_dir_sectors : bpb.sectors_per_cluster;
}

static void fat_build_name(unsigned char *raw_name, unsigned char *raw_ext,
                           char *dst) {
  int p = 0;
  if (raw_name[0] == '.') {
    dst[p++] = '.';
    if (raw_name[1] == '.')
      dst[p++] = '.';
    dst[p] = '\0';
    return;
  }
  for (int i = 0; i < 8 && raw_name[i] != ' '; i++) {
    char c = raw_name[i];
    if (c >= 'A' && c <= 'Z')
      c += 32;
    dst[p++] = c;
  }
  if (raw_ext[0] != ' ') {
    dst[p++] = '.';
    for (int i = 0; i < 3 && raw_ext[i] != ' '; i++) {
      char c = raw_ext[i];
      if (c >= 'A' && c <= 'Z')
        c += 32;
      dst[p++] = c;
    }
  }
  dst[p] = '\0';
}

static void fat_vfs_fill_ops(vfs_node_t *node, uint8_t attr) {
  if (attr & 0x10) {
    node->flags = FS_DIRECTORY;
    node->read = 0;
    node->write = 0;
    node->finddir = fat_vfs_finddir;
    node->readdir = fat_vfs_readdir;
    node->create = fat_vfs_create;
    node->unlink = fat_vfs_unlink;
    node->mkdir = fat_vfs_mkdir_vfs;
    node->rmdir = fat_vfs_rmdir_vfs;
    node->truncate = 0;
  } else {
    node->flags = FS_FILE;
    node->read = fat_vfs_read;
    node->write = fat_vfs_write;
    node->finddir = 0;
    node->readdir = 0;
    node->create = 0;
    node->unlink = 0;
    node->mkdir = 0;
    node->rmdir = 0;
    node->truncate = 0;
  }
}

// --- device_specific_data encoding ---
//
// Directories: device_specific_data is just their own cluster (uint32_t),
//   unpacked, exactly as before. mkdir/create/unlink/rmdir on a directory
//   node only ever need that directory's own cluster as the "parent" for
//   whatever child they're creating/removing — no further info needed.
//
// Files: device_specific_data PACKS two 16-bit cluster numbers into one
//   32-bit value: (parent_dir_cluster << 16) | file_first_cluster.
//   This lets fat_vfs_write() locate and update the file's directory entry
//   directly from the node, with NO dependency on the legacy global
//   current_dir_cluster — fixing writes to files outside the root directory.
#define FAT_PACK(parent, child)                                                \
  ((((uint32_t)(parent) & 0xFFFF) << 16) | ((uint32_t)(child) & 0xFFFF))
#define FAT_UNPACK_CHILD(packed) ((uint32_t)(packed) & 0xFFFF)
#define FAT_UNPACK_PARENT(packed) (((uint32_t)(packed) >> 16) & 0xFFFF)

static void fat_vfs_entry_to_node(struct fat_dir_entry *e, vfs_node_t *out,
                                  uint32_t parent_cluster) {
  fat_build_name(e->name, e->ext, out->name);
  out->size = e->size;
  if (e->attr & 0x10) {
    out->device_specific_data = (void *)(uint32_t)e->first_cluster_low;
  } else {
    out->device_specific_data =
        (void *)FAT_PACK(parent_cluster, e->first_cluster_low);
  }
  fat_vfs_fill_ops(out, e->attr);
}

static uint32_t fat_vfs_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                             uint8_t *buffer) {
  struct fat_dir_entry fake;
  fake.first_cluster_low =
      (uint16_t)FAT_UNPACK_CHILD((uint32_t)node->device_specific_data);
  fake.size = node->size;

  void *raw = fat_load_file(&fake);
  if (!raw)
    return 0;

  if (offset >= node->size) {
    kfree(raw);
    return 0;
  }

  uint32_t available = node->size - offset;
  uint32_t to_copy = (size < available) ? size : available;
  kmemcpy(buffer, (uint8_t *)raw + offset, to_copy);
  kfree(raw);
  return to_copy;
}

// Cluster-parameterised file write — locates the directory entry inside
// parent_cluster by name, grows/allocates clusters as needed, and writes
// the data. Mirrors fat_write_file_raw() but never touches the legacy
// current_dir_cluster global, so it works correctly regardless of what
// directory the shell currently has open.
static int fat_write_file_in_dir(uint32_t parent_cluster, const char *filename,
                                 const uint8_t *data, uint32_t total_size) {
  uint32_t dir_lba = fat_dir_lba(parent_cluster);

  uint8_t *dir_buf = (uint8_t *)kmalloc(512);
  if (!dir_buf)
    return -1;

  ide_read_sector(dir_lba, dir_buf);
  struct fat_dir_entry *entries = (struct fat_dir_entry *)dir_buf;

  int slot = -1;
  for (int i = 0; i < 16; i++) {
    if (fat_compare_name(filename, (char *)entries[i].name,
                         (char *)entries[i].ext)) {
      slot = i;
      break;
    }
  }
  if (slot == -1) {
    kfree(dir_buf);
    return -1; // file not found in this directory
  }

  uint16_t current_cluster = entries[slot].first_cluster_low;
  if (current_cluster < 2) {
    current_cluster = fat_find_free_cluster();
    if (current_cluster == 0xFFFF) {
      kfree(dir_buf);
      return -1;
    }
    entries[slot].first_cluster_low = current_cluster;
    fat_update_table(current_cluster, 0xFFFF);
  }

  entries[slot].size = total_size;
  ide_write_sector(dir_lba, dir_buf);
  kfree(dir_buf);

  if (total_size == 0)
    return 0;

  uint8_t *sector_scratch = (uint8_t *)kmalloc(512);
  if (!sector_scratch)
    return -1;

  uint32_t bytes_written = 0;
  while (bytes_written < total_size) {
    kmemset(sector_scratch, 0, 128);
    uint32_t chunk =
        (total_size - bytes_written > 512) ? 512 : (total_size - bytes_written);
    kmemcpy(sector_scratch, data + bytes_written, chunk);

    uint32_t sector_in_cluster =
        (bytes_written / 512) % bpb.sectors_per_cluster;
    uint32_t data_lba = cluster_to_lba(current_cluster) + sector_in_cluster;

    ide_write_sector(data_lba, sector_scratch);
    bytes_written += chunk;

    if (bytes_written < total_size &&
        (bytes_written % (bpb.sectors_per_cluster * 512) == 0)) {
      uint16_t next = fat_get_next_cluster(current_cluster);
      if (next >= 0xFFF8 || next < 2) {
        next = fat_find_free_cluster();
        if (next == 0xFFFF) {
          kfree(sector_scratch);
          return -1;
        }
        fat_update_table(current_cluster, next);
        fat_update_table(next, 0xFFFF);
      }
      current_cluster = next;
    }
  }

  kfree(sector_scratch);
  return 0;
}

static uint32_t fat_vfs_write(vfs_node_t *node, uint32_t offset, uint32_t size,
                              uint8_t *buffer) {
  (void)offset; // overwrite-from-0 semantics, matching fat_write_file_raw
  uint32_t parent_cluster =
      FAT_UNPACK_PARENT((uint32_t)node->device_specific_data);

  if (fat_write_file_in_dir(parent_cluster, node->name, buffer, size) != 0) {
    return 0; // write failed — caller (cmd_write) reports bytes actually
              // written
  }

  node->size = size;
  return size;
}

static vfs_node_t *fat_vfs_finddir(vfs_node_t *node, char *name) {
  uint32_t cluster = (uint32_t)node->device_specific_data;

  struct fat_dir_entry *entry = fat_search_in(name, cluster);
  if (!entry)
    return NULL;

  vfs_node_t *out = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
  if (!out)
    return NULL;
  fat_vfs_entry_to_node(entry, out, cluster);
  return out;
}

static int fat_vfs_readdir(vfs_node_t *node, uint32_t index, vfs_node_t *out) {
  uint32_t cluster = (uint32_t)node->device_specific_data;
  uint32_t base_lba = fat_dir_lba(cluster);
  uint32_t sectors = fat_dir_sectors(cluster);

  uint32_t valid_index = 0;
  uint8_t buf[512];

  for (uint32_t s = 0; s < sectors; s++) {
    ide_read_sector(base_lba + s, buf);
    struct fat_dir_entry *entries = (struct fat_dir_entry *)buf;

    for (int i = 0; i < 16; i++) {
      if (entries[i].name[0] == 0x00)
        return 0;
      if ((unsigned char)entries[i].name[0] == 0xE5)
        continue;
      if (entries[i].attr == 0x0F)
        continue;

      if (valid_index == index) {
        fat_vfs_entry_to_node(&entries[i], out, cluster);
        return 1;
      }
      valid_index++;
    }
  }
  return 0;
}

static int fat_vfs_create(vfs_node_t *parent, char *name, uint32_t flags) {
  (void)flags;
  uint32_t cluster = (uint32_t)parent->device_specific_data;
  uint32_t dir_lba = fat_dir_lba(cluster);

  uint8_t *dir_buf = (uint8_t *)kmalloc(512);
  if (!dir_buf)
    return -1;

  ide_read_sector(dir_lba, dir_buf);
  struct fat_dir_entry *entries = (struct fat_dir_entry *)dir_buf;

  // Reject duplicate names instead of creating a second entry.
  for (int i = 0; i < 16; i++) {
    if (entries[i].name[0] == 0x00)
      break;
    if ((unsigned char)entries[i].name[0] == 0xE5)
      continue;
    if (fat_compare_name(name, (char *)entries[i].name,
                         (char *)entries[i].ext)) {
      kfree(dir_buf);
      return 0; // already exists — treat as success, caller can finddir it
    }
  }

  int slot = -1;
  for (int i = 0; i < 16; i++) {
    if (entries[i].name[0] == 0x00 ||
        (unsigned char)entries[i].name[0] == 0xE5) {
      slot = i;
      break;
    }
  }
  if (slot == -1) {
    kfree(dir_buf);
    return -1;
  }

  kmemset(&entries[slot], 0, sizeof(struct fat_dir_entry) / 4);
  for (int i = 0; i < 8; i++)
    entries[slot].name[i] = ' ';
  for (int i = 0; i < 3; i++)
    entries[slot].ext[i] = ' ';

  int dot = -1;
  for (int i = 0; name[i]; i++)
    if (name[i] == '.') {
      dot = i;
      break;
    }
  int name_len = (dot == -1) ? (int)kstrlen(name) : dot;
  for (int i = 0; i < name_len && i < 8; i++) {
    char c = name[i];
    if (c >= 'a' && c <= 'z')
      c -= 32;
    entries[slot].name[i] = c;
  }
  if (dot != -1) {
    for (int i = 0; i < 3 && name[dot + 1 + i]; i++) {
      char c = name[dot + 1 + i];
      if (c >= 'a' && c <= 'z')
        c -= 32;
      entries[slot].ext[i] = c;
    }
  }

  entries[slot].attr = 0x00;
  entries[slot].first_cluster_low = 0;
  entries[slot].size = 0;

  ide_write_sector(dir_lba, dir_buf);
  kfree(dir_buf);
  return 0;
}

static int fat_vfs_unlink(vfs_node_t *parent, char *name) {
  uint32_t cluster = (uint32_t)parent->device_specific_data;
  uint32_t dir_lba = fat_dir_lba(cluster);

  uint8_t buf[512];
  ide_read_sector(dir_lba, buf);
  struct fat_dir_entry *entries = (struct fat_dir_entry *)buf;

  for (int i = 0; i < 16; i++) {
    if (!fat_compare_name(name, (char *)entries[i].name,
                          (char *)entries[i].ext))
      continue;
    if (entries[i].attr & 0x10)
      return -1;

    uint16_t cl = entries[i].first_cluster_low;
    while (cl > 1 && cl < 0xFFF8) {
      uint16_t next = fat_get_next_cluster(cl);
      fat_update_table(cl, 0x0000);
      cl = next;
    }

    entries[i].name[0] = 0xE5;
    ide_write_sector(dir_lba, buf);
    return 0;
  }
  return -1;
}

static int fat_vfs_mkdir_vfs(vfs_node_t *parent, char *name) {
  uint32_t parent_cluster = (uint32_t)parent->device_specific_data;

  uint16_t new_cluster = fat_find_free_cluster();
  if (new_cluster == 0xFFFF)
    return -1;

  uint8_t *new_dir = (uint8_t *)kmalloc(1024);
  if (!new_dir)
    return -1;
  kmemset(new_dir, 0, 512 / 4);

  struct fat_dir_entry *dot = (struct fat_dir_entry *)new_dir;
  kmemcpy(dot[0].name, ".       ", 8);
  dot[0].attr = 0x10;
  dot[0].first_cluster_low = new_cluster;

  kmemcpy(dot[1].name, "..      ", 8);
  dot[1].attr = 0x10;
  dot[1].first_cluster_low = (uint16_t)parent_cluster;

  ide_write_sector(cluster_to_lba(new_cluster), new_dir);
  fat_update_table(new_cluster, 0xFFFF);
  kfree(new_dir);

  uint32_t dir_lba = fat_dir_lba(parent_cluster);
  uint8_t *dir_buf = (uint8_t *)kmalloc(1024);
  if (!dir_buf)
    return -1;

  ide_read_sector(dir_lba, dir_buf);
  struct fat_dir_entry *entries = (struct fat_dir_entry *)dir_buf;

  int slot = -1;
  for (int i = 0; i < 16; i++) {
    if (entries[i].name[0] == 0x00 ||
        (unsigned char)entries[i].name[0] == 0xE5) {
      slot = i;
      break;
    }
  }
  if (slot == -1) {
    kfree(dir_buf);
    return -1;
  }

  kmemset(&entries[slot], 0, sizeof(struct fat_dir_entry) / 4);
  for (int i = 0; i < 8; i++)
    entries[slot].name[i] = ' ';
  for (int i = 0; i < 3; i++)
    entries[slot].ext[i] = ' ';
  for (int i = 0; i < 8 && name[i]; i++) {
    char c = name[i];
    if (c >= 'a' && c <= 'z')
      c -= 32;
    entries[slot].name[i] = c;
  }
  entries[slot].attr = 0x10;
  entries[slot].first_cluster_low = new_cluster;
  entries[slot].size = 0;

  ide_write_sector(dir_lba, dir_buf);
  kfree(dir_buf);
  return 0;
}

static int fat_vfs_rmdir_vfs(vfs_node_t *parent, char *name) {
  if (kstrcmp(name, ".") == 0 || kstrcmp(name, "..") == 0)
    return -1;

  uint32_t cluster = (uint32_t)parent->device_specific_data;
  uint32_t dir_lba = fat_dir_lba(cluster);

  uint8_t buf[512];
  ide_read_sector(dir_lba, buf);
  struct fat_dir_entry *entries = (struct fat_dir_entry *)buf;

  for (int i = 0; i < 16; i++) {
    if (!fat_compare_name(name, (char *)entries[i].name,
                          (char *)entries[i].ext))
      continue;
    if (!(entries[i].attr & 0x10))
      return -1;

    uint16_t cl = entries[i].first_cluster_low;
    if (cl > 1)
      fat_update_table(cl, 0x0000);

    entries[i].name[0] = 0xE5;
    ide_write_sector(dir_lba, buf);
    return 0;
  }
  return -1;
}

void fat_vfs_mount(void) {
  fs_root = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
  kstrcpy(fs_root->name, "/");
  fs_root->size = 0;
  fs_root->device_specific_data = (void *)0;
  fat_vfs_fill_ops(fs_root, 0x10);
  fs_root->flags |= FS_MOUNTPOINT;

  fs_current_dir = fs_root;
}
