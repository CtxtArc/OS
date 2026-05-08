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
    char clean_line[128];
    int j = 0;
    for (int i = 0; line[i] != '\0' && i < 127; i++) {
        if (line[i] >= 32 && line[i] <= 126) clean_line[j++] = line[i];
        else if (line[i] == '\n' || line[i] == '\t' || line[i] == '\r') clean_line[j++] = ' ';
    }
    clean_line[j] = '\0';

    for (int i = 0; clean_line[i] != '\0'; i++) {
        if (clean_line[i] == '#') { clean_line[i] = '\0'; break; }
    }

    char cmd[32];
    const char* ptr = get_token(clean_line, cmd);
    if (!ptr || cmd[0] == '\0') return;

    if (kstrcmp(cmd, "LABEL") == 0) {
        if (pass == 1) {
            char name[32]; get_token(ptr, name);
            if (label_count < 128) {
                kstrncpy(label_table[label_count].name, name, 32);
                label_table[label_count].address = *pos;
                label_count++;
            }
        }
        return; 
    } 

    if (kstrcmp(cmd, "GOTO") == 0) {
        char name[32]; get_token(ptr, name);
        uint32_t target = 0;
        if (pass == 2) {
            int found = 0;
            for(int i = 0; i < label_count; i++) {
                if(kstrcmp(label_table[i].name, name) == 0) { 
                    target = label_table[i].address; 
                    found = 1;
                    break; 
                }
            }
            if (!found) kprintf_unsync("ASM ERROR: GOTO Label '%s' not found!\n", name);
        }
        if (out_buf) out_buf[*pos] = 0xE9; 
        (*pos)++;
        int32_t offset = (int32_t)target - (int32_t)(*pos + 4);
        if (out_buf) kmemcpy(&out_buf[*pos], &offset, 4); 
        *pos += 4;
    }
    else if (kstrcmp(cmd, "SET") == 0) {
        char var_name[32], val_str[32];
        ptr = get_token(ptr, var_name); ptr = get_token(ptr, val_str);
        uint32_t offset = get_var_offset(var_name);
        uint32_t val = (val_str[0] == '0' && val_str[1] == 'x') ? katoh(val_str) : (uint32_t)katoi((char*)val_str);
        
        if (out_buf) { out_buf[*pos] = 0xC7; out_buf[*pos+1] = 0x05; }
        *pos += 2;
        if (out_buf) { kmemcpy(&out_buf[*pos], &offset, 4); }
        *pos += 4;
        if (out_buf) { kmemcpy(&out_buf[*pos], &val, 4); }
        *pos += 4;
    }
    else if (kstrcmp(cmd, "ADD") == 0 || kstrcmp(cmd, "SUB") == 0) {
        char var_name[32], val_str[32];
        ptr = get_token(ptr, var_name); ptr = get_token(ptr, val_str);
        uint32_t offset = get_var_offset(var_name);
        uint32_t amount = (uint32_t)katoi((char*)val_str);
        
        if (out_buf) { 
            out_buf[*pos] = 0x81; 
            out_buf[*pos+1] = (kstrcmp(cmd, "ADD") == 0) ? 0x05 : 0x2D; 
        }
        *pos += 2;
        if (out_buf) { kmemcpy(&out_buf[*pos], &offset, 4); }
        *pos += 4;
        if (out_buf) { kmemcpy(&out_buf[*pos], &amount, 4); }
        *pos += 4;
    }
    else if (kstrcmp(cmd, "PRINT") == 0) {
        const char* p = ptr;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\"') {
            char str_val[128]; p++; uint32_t s_len = 0;
            while (*p != '\"' && *p != '\0') str_val[s_len++] = *p++;
            str_val[s_len++] = '\0'; if (*p == '\"') p++;
            
            char t1[32], t2[32], t3[32];
            p = get_token(p, t1); p = get_token(p, t2); p = get_token(p, t3);
            
            if (out_buf) { out_buf[*pos] = 0xEB; out_buf[*pos+1] = (uint8_t)s_len; }
            *pos += 2;
            
            uint32_t str_off = *pos;
            if (out_buf) { for(uint32_t j=0; j<s_len; j++) out_buf[*pos + j] = (uint8_t)str_val[j]; }
            *pos += s_len;
            
            emit_load(0xB8, "7", out_buf, pos);
            emit_mov(0xBB, str_off, out_buf, pos);
            emit_load(0xB9, t1, out_buf, pos);
            emit_load(0xBA, t2, out_buf, pos);
            emit_load(0xBF, t3, out_buf, pos);
            
            if (out_buf) { out_buf[*pos] = 0xCD; out_buf[*pos+1] = 0x80; }
            *pos += 2;
        } else {
            char var[32], t1[32], t2[32], t3[32];
            ptr = get_token(ptr, var); ptr = get_token(ptr, t1);
            ptr = get_token(ptr, t2); ptr = get_token(ptr, t3);
            emit_load(0xB8, "9", out_buf, pos);
            uint32_t off = get_var_offset(var);
            if (out_buf) { out_buf[*pos] = 0x8B; out_buf[*pos+1] = 0x1D; }
            *pos += 2;
            if (out_buf) { kmemcpy(&out_buf[*pos], &off, 4); }
            *pos += 4;
            emit_load(0xB9, t1, out_buf, pos);
            emit_load(0xBA, t2, out_buf, pos);
            emit_load(0xBF, t3, out_buf, pos);
            if (out_buf) { out_buf[*pos] = 0xCD; out_buf[*pos+1] = 0x80; }
            *pos += 2;
        }
    }
    else if (kstrcmp(cmd, "CMP") == 0) {
        char var[32], val_s[32];
        ptr = get_token(ptr, var); ptr = get_token(ptr, val_s);
        uint32_t off = get_var_offset(var);
        uint32_t val = (val_s[0]=='0' && val_s[1]=='x') ? katoh(val_s) : (uint32_t)katoi((char*)val_s);
        if (out_buf) out_buf[*pos] = 0xA1; 
        (*pos)++;
        if (out_buf) kmemcpy(&out_buf[*pos], &off, 4); *pos += 4;
        if (out_buf) out_buf[*pos] = 0x3D; 
        (*pos)++;
        if (out_buf) kmemcpy(&out_buf[*pos], &val, 4); *pos += 4;
    }
    else if (cmd[0] == 'J' && (cmd[1] == 'E' || cmd[1] == 'L' || cmd[1] == 'G')) {
        char lbl[32]; get_token(ptr, lbl);
        uint8_t cond = (cmd[1]=='E') ? 0x84 : (cmd[1]=='L' ? 0x8C : 0x8F);
        uint32_t target = 0;
        if (pass == 2) {
            int found = 0;
            for(int i=0; i<label_count; i++) {
                if(kstrcmp(label_table[i].name, lbl) == 0) { 
                    target = label_table[i].address; 
                    found = 1;
                    break; 
                }
            }
            if (!found) kprintf_unsync("ASM ERROR: Jump Label '%s' not found!\n", lbl);
        }
        if (out_buf) { out_buf[*pos] = 0x0F; out_buf[*pos+1] = cond; }
        *pos += 2;
        int32_t off = (int32_t)target - (int32_t)(*pos + 4);
        if (out_buf) kmemcpy(&out_buf[*pos], &off, 4); 
        *pos += 4;
    }
    else if (kstrcmp(cmd, "SLEEP") == 0) {
        char val[32]; get_token(ptr, val);
        emit_load(0xB8, "3", out_buf, pos);
        emit_load(0xBB, val, out_buf, pos);
        if (out_buf) { out_buf[*pos] = 0xCD; out_buf[*pos+1] = 0x80; }
        *pos += 2;
    }
    else if (kstrcmp(cmd, "CLEAR") == 0 || kstrcmp(cmd, "EXIT") == 0) {
        emit_load(0xB8, (kstrcmp(cmd, "CLEAR") == 0) ? "5" : "4", out_buf, pos);
        if (out_buf) { out_buf[*pos] = 0xCD; out_buf[*pos+1] = 0x80; }
        *pos += 2;
    }
    else if (kstrcmp(cmd, "WINDOW") == 0) {
        char t1[32], t2[32], t3[32], t4[32];
        ptr = get_token(ptr, t1); ptr = get_token(ptr, t2);
        ptr = get_token(ptr, t3); ptr = get_token(ptr, t4);
        emit_load(0xB8, "8", out_buf, pos);
        emit_load(0xBB, t1, out_buf, pos);
        emit_load(0xB9, t2, out_buf, pos);
        emit_load(0xBA, t3, out_buf, pos);
        emit_load(0xBE, t4, out_buf, pos);
        if (out_buf) { out_buf[*pos] = 0xCD; out_buf[*pos+1] = 0x80; }
        *pos += 2;
    }
    else if (kstrcmp(cmd, "RECT") == 0) {
        char t1[32], t2[32], t3[32], t4[32], t5[32];
        ptr = get_token(ptr, t1); ptr = get_token(ptr, t2);
        ptr = get_token(ptr, t3); ptr = get_token(ptr, t4);
        ptr = get_token(ptr, t5);
        emit_load(0xB8, "6", out_buf, pos);
        emit_load(0xBB, t1, out_buf, pos);
        emit_load(0xB9, t2, out_buf, pos);
        emit_load(0xBA, t3, out_buf, pos);
        emit_load(0xBE, t4, out_buf, pos);
        emit_load(0xBF, t5, out_buf, pos);
        if (out_buf) { out_buf[*pos] = 0xCD; out_buf[*pos+1] = 0x80; }
        *pos += 2;
    }
}
