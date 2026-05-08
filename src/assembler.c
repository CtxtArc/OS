#include "assembler.h"
#include "lib.h"
#include "idt.h"
#include "kheap.h"

Label label_table[128]; 
int label_count = 0;
RuntimeVar rt_vars[64];
int rt_var_count = 0;

uint32_t get_var_offset(const char* name) {
    for(int i = 0; i < rt_var_count; i++) {
        if(kstrcmp(rt_vars[i].name, name) == 0) return rt_vars[i].offset;
    }
    uint32_t new_offset = 3000 + (rt_var_count * 4);
    kstrncpy(rt_vars[rt_var_count].name, name, 32);
    rt_vars[rt_var_count].offset = new_offset;
    rt_var_count++;
    return new_offset;
}

void assemble_line(const char* line, uint8_t* out_buf, uint32_t* pos, int pass) {
    // --- 1. SANITIZATION (Fixes Selection_016.png issues) ---
    char clean_line[128];
    int j = 0;
    for (int i = 0; line[i] != '\0' && i < 127; i++) {
        if (line[i] >= 32 && line[i] <= 126) {
            clean_line[j++] = line[i];
        } else if (line[i] == '\n' || line[i] == '\t' || line[i] == '\r') {
            clean_line[j++] = ' ';
        }
    }
    clean_line[j] = '\0';

    for (int i = 0; clean_line[i] != '\0'; i++) {
        if (clean_line[i] == '#') { clean_line[i] = '\0'; break; }
    }

    char cmd[32];
    const char* ptr = get_token(clean_line, cmd);
    if (!ptr || cmd[0] == '\0') return;

    // --- 2. COMMAND PARSING ---
    if (kstrcmp(cmd, "LABEL") == 0) {
        if (pass == 1) {
            char name[32];
            get_token(ptr, name);
            if (label_count < 128) {
                kstrncpy(label_table[label_count].name, name, 32);
                label_table[label_count].address = *pos;
                label_count++;
            }
        }
    } 
    else if (kstrcmp(cmd, "GOTO") == 0) {
        if (pass == 2) {
            char name[32]; get_token(ptr, name);
            uint32_t target = 0;
            for(int i = 0; i < label_count; i++) {
                if(kstrcmp(label_table[i].name, name) == 0) { target = label_table[i].address; break; }
            }
            out_buf[(*pos)++] = 0xE9; 
            int32_t offset = (int32_t)target - (int32_t)(*pos + 4);
            kmemcpy(&out_buf[*pos], &offset, 4); *pos += 4;
        } else { *pos += 5; }
    }
    else if (kstrcmp(cmd, "SET") == 0) {
        if (pass == 2) {
            char var_name[32], val_str[32];
            ptr = get_token(ptr, var_name); ptr = get_token(ptr, val_str);
            uint32_t offset = get_var_offset(var_name);
            uint32_t val = (val_str[0] == '0' && val_str[1] == 'x') ? katoh(val_str) : (uint32_t)katoi(val_str);
            out_buf[(*pos)++] = 0xC7; out_buf[(*pos)++] = 0x05;
            kmemcpy(&out_buf[*pos], &offset, 4); *pos += 4;
            kmemcpy(&out_buf[*pos], &val, 4);    *pos += 4;
        } else { *pos += 10; }
    }
    else if (kstrcmp(cmd, "PRINT") == 0) {
        const char* p = ptr;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\"') {
            // String logic (Unchanged)
            char str_val[128]; p++; uint32_t s_len = 0;
            while (*p != '\"' && *p != '\0') str_val[s_len++] = *p++;
            str_val[s_len++] = '\0'; if (*p == '\"') p++;
            if (pass == 2) {
                char t1[32], t2[32], t3[32];
                p = get_token(p, t1); p = get_token(p, t2); p = get_token(p, t3);
                out_buf[(*pos)++] = 0xEB; out_buf[(*pos)++] = (uint8_t)s_len;
                uint32_t str_off = *pos;
                for(uint32_t j=0; j<s_len; j++) out_buf[(*pos)++] = (uint8_t)str_val[j];
                emit_load(0xB8, "7", out_buf, pos);
                emit_mov(0xBB, str_off, out_buf, pos);
                emit_load(0xB9, t1, out_buf, pos);
                emit_load(0xBA, t2, out_buf, pos);
                emit_load(0xBF, t3, out_buf, pos);
                out_buf[(*pos)++] = 0xCD; out_buf[(*pos)++] = 0x80;
            } else { *pos +=  (32); }
        } else {
            // Variable logic (FIXED: Load VALUE into EBX)
            if (pass == 2) {
                char var[32], t1[32], t2[32], t3[32];
                ptr = get_token(ptr, var); ptr = get_token(ptr, t1);
                ptr = get_token(ptr, t2); ptr = get_token(ptr, t3);
                emit_load(0xB8, "9", out_buf, pos);
                uint32_t off = get_var_offset(var);
                out_buf[(*pos)++] = 0x8B; out_buf[(*pos)++] = 0x1D; // MOV EBX, [mem]
                kmemcpy(&out_buf[*pos], &off, 4); *pos += 4;
                emit_load(0xB9, t1, out_buf, pos);
                emit_load(0xBA, t2, out_buf, pos);
                emit_load(0xBF, t3, out_buf, pos);
                out_buf[(*pos)++] = 0xCD; out_buf[(*pos)++] = 0x80;
            } else { *pos += 32; }
        }
    }
    else if (kstrcmp(cmd, "CMP") == 0) {
        if (pass == 2) {
            char var[32], val_s[32];
            ptr = get_token(ptr, var); ptr = get_token(ptr, val_s);
            uint32_t off = get_var_offset(var);
            uint32_t val = (val_s[0]=='0' && val_s[1]=='x') ? katoh(val_s) : (uint32_t)katoi(val_s);
            out_buf[(*pos)++] = 0xA1; kmemcpy(&out_buf[*pos], &off, 4); *pos += 4;
            out_buf[(*pos)++] = 0x3D; kmemcpy(&out_buf[*pos], &val, 4); *pos += 4;
        } else { *pos += 10; }
    }
    else if (cmd[0] == 'J' && (cmd[1] == 'E' || cmd[1] == 'L' || cmd[1] == 'G')) {
        if (pass == 2) {
            char lbl[32]; get_token(ptr, lbl);
            uint8_t cond = (cmd[1]=='E') ? 0x84 : (cmd[1]=='L' ? 0x8C : 0x8F);
            uint32_t target = 0;
            for(int i=0; i<label_count; i++) {
                if(kstrcmp(label_table[i].name, lbl) == 0) { target = label_table[i].address; break; }
            }
            out_buf[(*pos)++] = 0x0F; out_buf[(*pos)++] = cond;
            int32_t off = (int32_t)target - (int32_t)(*pos + 4);
            kmemcpy(&out_buf[*pos], &off, 4); *pos += 4;
        } else { *pos += 6; }
    }
    else if (kstrcmp(cmd, "SLEEP") == 0) {
        if (pass == 2) {
            char val[32]; get_token(ptr, val);
            emit_load(0xB8, "3", out_buf, pos);
            emit_load(0xBB, val, out_buf, pos);
            out_buf[(*pos)++] = 0xCD; out_buf[(*pos)++] = 0x80;
        } else { *pos += 12; }
    }
    else if (kstrcmp(cmd, "CLEAR") == 0 || kstrcmp(cmd, "EXIT") == 0) {
        if (pass == 2) {
            emit_load(0xB8, (kstrcmp(cmd, "CLEAR") == 0) ? "5" : "4", out_buf, pos);
            out_buf[(*pos)++] = 0xCD; out_buf[(*pos)++] = 0x80;
        } else { *pos += 8; }
    }
}
