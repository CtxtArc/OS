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
            
            emit_load(0xB8, "9", out_buf, pos); // EAX = 9
            
            // NEW: Load the VALUE of the variable into EBX
            uint32_t off = get_var_offset(var);
            if (out_buf) { out_buf[*pos] = 0x8B; out_buf[*pos+1] = 0x1D; kmemcpy(&out_buf[*pos+2], &off, 4); }
            *pos += 6;

            emit_load(0xB9, t1, out_buf, pos); // ECX = X
            emit_load(0xBA, t2, out_buf, pos); // EDX = Y
            emit_load(0xBF, t3, out_buf, pos); // EDI = Color
            
            if (out_buf) { out_buf[*pos] = 0xCD; out_buf[*pos+1] = 0x80; }
            *pos += 2;
        }    }
    else if (kstrcmp(cmd, "CMP") == 0) {
        char var[32], val_s[32];
        ptr = get_token(ptr, var); ptr = get_token(ptr, val_s);
        uint32_t off = get_var_offset(var);
        
        // 1. Load the first variable into EAX
        if (out_buf) { out_buf[*pos] = 0xA1; kmemcpy(&out_buf[*pos+1], &off, 4); } 
        *pos += 5;
        
        // 2. Check if the second argument is a number or a variable
        if ((val_s[0] >= '0' && val_s[0] <= '9') || val_s[0] == '-') {
            // Compare EAX to Immediate Number
            uint32_t val = (val_s[0]=='0' && val_s[1]=='x') ? katoh(val_s) : (uint32_t)katoi((char*)val_s);
            if (out_buf) { out_buf[*pos] = 0x3D; kmemcpy(&out_buf[*pos+1], &val, 4); } 
            *pos += 5;
        } else {
            // Compare EAX to Second Variable's Memory Value
            uint32_t off2 = get_var_offset(val_s);
            if (out_buf) { out_buf[*pos] = 0x3B; out_buf[*pos+1] = 0x05; kmemcpy(&out_buf[*pos+2], &off2, 4); } 
            *pos += 6;
        }
    }
    // FIX: ADDED 'N' FOR JNE (Jump Not Equal)
    else if (cmd[0] == 'J' && (cmd[1] == 'E' || cmd[1] == 'L' || cmd[1] == 'G' || cmd[1] == 'N')) {
        char lbl[32]; get_token(ptr, lbl);
        uint8_t cond = 0;
        if (cmd[1] == 'E') cond = 0x84;      // JE  (Equal)
        else if (cmd[1] == 'L') cond = 0x8C; // JL  (Less)
        else if (cmd[1] == 'G') cond = 0x8F; // JG  (Greater)
        else if (cmd[1] == 'N') cond = 0x85; // JNE (Not Equal)
        
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
    // --- NEW: SUBROUTINES (CALL / RET) ---
    else if (kstrcmp(cmd, "CALL") == 0) {
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
            if (!found) kprintf_unsync("ASM ERROR: CALL Label '%s' not found!\n", name);
        }
        if (out_buf) out_buf[*pos] = 0xE8; // E8 is x86 CALL
        (*pos)++;
        int32_t offset = (int32_t)target - (int32_t)(*pos + 4);
        if (out_buf) kmemcpy(&out_buf[*pos], &offset, 4); 
        *pos += 4;
    }
    else if (kstrcmp(cmd, "RET") == 0) {
        if (out_buf) out_buf[*pos] = 0xC3; // C3 is x86 RET
        (*pos)++;
    }
    // --- NEW: KEYBOARD INPUT (GETKEY) ---
    else if (kstrcmp(cmd, "GETKEY") == 0) {
        char var_name[32]; get_token(ptr, var_name);
        uint32_t offset = get_var_offset(var_name);
        
        emit_load(0xB8, "10", out_buf, pos); // Load 10 into EAX for Syscall 10
        if (out_buf) { out_buf[*pos] = 0xCD; out_buf[*pos+1] = 0x80; } // INT 0x80
        *pos += 2;
        
        if (out_buf) { out_buf[*pos] = 0xA3; } // A3 is MOV [absolute_memory], EAX
        (*pos)++;
        if (out_buf) { kmemcpy(&out_buf[*pos], &offset, 4); }
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
// --- NEW: LOADFILE "filename.ext" ptr_var size_var ---
    else if (kstrcmp(cmd, "LOADFILE") == 0) {
        const char* p = ptr;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\"') {
            char str_val[128]; p++; uint32_t s_len = 0;
            while (*p != '\"' && *p != '\0') str_val[s_len++] = *p++;
            str_val[s_len++] = '\0'; if (*p == '\"') p++;
            
            char var_ptr[32], var_size[32];
            p = get_token(p, var_ptr); p = get_token(p, var_size);
            
            // 1. Write the string into the binary
            if (out_buf) { out_buf[*pos] = 0xEB; out_buf[*pos+1] = (uint8_t)s_len; }
            *pos += 2;
            uint32_t str_off = *pos;
            if (out_buf) { for(uint32_t j=0; j<s_len; j++) out_buf[*pos + j] = (uint8_t)str_val[j]; }
            *pos += s_len;
            
            // 2. Call Syscall 11
            emit_load(0xB8, "11", out_buf, pos); // EAX = 11
            emit_mov(0xBB, str_off, out_buf, pos); // EBX = string offset
            if (out_buf) { out_buf[*pos] = 0xCD; out_buf[*pos+1] = 0x80; } // INT 0x80
            *pos += 2;
            
            // 3. Save EAX (Memory Pointer) into var_ptr
            uint32_t off_ptr = get_var_offset(var_ptr);
            if (out_buf) { out_buf[*pos] = 0xA3; } // MOV [addr], EAX
            (*pos)++;
            if (out_buf) { kmemcpy(&out_buf[*pos], &off_ptr, 4); }
            *pos += 4;
            
            // 4. Save ECX (File Size) into var_size
            uint32_t off_size = get_var_offset(var_size);
            if (out_buf) { out_buf[*pos] = 0x89; out_buf[*pos+1] = 0x0D; } // MOV [addr], ECX
            *pos += 2;
            if (out_buf) { kmemcpy(&out_buf[*pos], &off_size, 4); }
            *pos += 4;
        }
    }
    // --- NEW: READBYTE ptr_var index_var out_var ---
    // Reads a single byte from (ptr_var + index_var) into out_var
    else if (kstrcmp(cmd, "READBYTE") == 0) {
        char v_ptr[32], v_idx[32], v_out[32];
        ptr = get_token(ptr, v_ptr); ptr = get_token(ptr, v_idx); ptr = get_token(ptr, v_out);
        uint32_t off_ptr = get_var_offset(v_ptr);
        uint32_t off_idx = get_var_offset(v_idx);
        uint32_t off_out = get_var_offset(v_out);

        // mov eax, [v_ptr]
        if (out_buf) { out_buf[*pos] = 0xA1; kmemcpy(&out_buf[*pos+1], &off_ptr, 4); } *pos += 5;
        // mov ecx, [v_idx]
        if (out_buf) { out_buf[*pos] = 0x8B; out_buf[*pos+1] = 0x0D; kmemcpy(&out_buf[*pos+2], &off_idx, 4); } *pos += 6;
        // add eax, ecx
        if (out_buf) { out_buf[*pos] = 0x01; out_buf[*pos+1] = 0xC8; } *pos += 2;
        // xor ebx, ebx (Clear EBX so we don't have garbage data)
        if (out_buf) { out_buf[*pos] = 0x31; out_buf[*pos+1] = 0xDB; } *pos += 2;
        // mov bl, byte [eax]
        if (out_buf) { out_buf[*pos] = 0x8A; out_buf[*pos+1] = 0x18; } *pos += 2;
        // mov [v_out], ebx
        if (out_buf) { out_buf[*pos] = 0x89; out_buf[*pos+1] = 0x1D; kmemcpy(&out_buf[*pos+2], &off_out, 4); } *pos += 6;
    }
    // --- NEW: FREE ptr_var ---
    else if (kstrcmp(cmd, "FREE") == 0) {
        char v_ptr[32]; get_token(ptr, v_ptr);
        uint32_t off_ptr = get_var_offset(v_ptr);
        
        // mov ebx, [v_ptr]
        if (out_buf) { out_buf[*pos] = 0x8B; out_buf[*pos+1] = 0x1D; kmemcpy(&out_buf[*pos+2], &off_ptr, 4); } *pos += 6;
        // mov eax, 12 ; int 0x80
        if (out_buf) { out_buf[*pos] = 0xB8; out_buf[*pos+1] = 0x0C; out_buf[*pos+2] = 0x00; out_buf[*pos+3] = 0x00; out_buf[*pos+4] = 0x00; } *pos += 5;
        if (out_buf) { out_buf[*pos] = 0xCD; out_buf[*pos+1] = 0x80; } *pos += 2;
    }
else if (kstrcmp(cmd, "PRINT_CHAR") == 0) {
    // PRINT_CHAR char_var x_var y_var color
    char v_c[32], v_x[32], v_y[32], v_col[32];
    ptr = get_token(ptr, v_c);
    ptr = get_token(ptr, v_x);
    ptr = get_token(ptr, v_y);
    ptr = get_token(ptr, v_col);

    uint32_t off_c   = get_var_offset(v_c);
    uint32_t off_x   = get_var_offset(v_x);
    uint32_t off_y   = get_var_offset(v_y);

    // mov ebx, [char_var]  — the actual byte value
    if (out_buf) { out_buf[*pos]=0x8B; out_buf[*pos+1]=0x1D;
                   kmemcpy(&out_buf[*pos+2], &off_c, 4); } *pos += 6;
    // mov ecx, [x_var]
    if (out_buf) { out_buf[*pos]=0x8B; out_buf[*pos+1]=0x0D;
                   kmemcpy(&out_buf[*pos+2], &off_x, 4); } *pos += 6;
    // mov edx, [y_var]
    if (out_buf) { out_buf[*pos]=0x8B; out_buf[*pos+1]=0x15;
                   kmemcpy(&out_buf[*pos+2], &off_y, 4); } *pos += 6;
    // mov edi, <color literal>  — parse v_col as hex/dec
    uint32_t col_val = parse_literal(v_col);
    if (out_buf) { out_buf[*pos]=0xBF; kmemcpy(&out_buf[*pos+1], &col_val, 4); } *pos += 5;
    // mov eax, 13
    if (out_buf) { out_buf[*pos]=0xB8; out_buf[*pos+1]=0x0D;
                   out_buf[*pos+2]=0; out_buf[*pos+3]=0; out_buf[*pos+4]=0; } *pos += 5;
    // int 0x80
    if (out_buf) { out_buf[*pos]=0xCD; out_buf[*pos+1]=0x80; } *pos += 2;
}
}
