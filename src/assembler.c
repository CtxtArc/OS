#include "assembler.h"
#include "lib.h"
#include "idt.h"
#include "kheap.h"
#include "assembler.h"


Label label_table[64];
int label_count = 0;

RuntimeVar rt_vars[64];
int rt_var_count = 0;


// Helper to find or create a variable's memory offset
uint32_t get_var_offset(const char* name) {
    for(int i = 0; i < rt_var_count; i++) {
        if(kstrcmp(rt_vars[i].name, name) == 0) return rt_vars[i].offset;
    }
    // Create new variable at the end of the 4KB buffer (e.g., starting at 3000)
    uint32_t new_offset = 3000 + (rt_var_count * 4);
    kstrncpy(rt_vars[rt_var_count].name, name, 32);
    rt_vars[rt_var_count].offset = new_offset;
    rt_var_count++;
    return new_offset;
}

void assemble_line(const char* line, uint8_t* out_buf, uint32_t* pos, int pass) {
    char clean_line[128];
    kstrncpy(clean_line, line, 128);

    // Find first # and terminate string there
    for (int i = 0; i < 128 && clean_line[i] != '\0'; i++) {
        if (clean_line[i] == '#') {
            clean_line[i] = '\0';
            break;
        }
    }
    char cmd[32];
char arg_str[32]; // Fixed: Declared here for general use
    // Use clean_line from here on instead of line
    const char* ptr = get_token(clean_line, cmd);
    if (!ptr || cmd[0] == '\0') return;







    // --- LABEL: No code, just recording address ---
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
        return; 
    }

    // --- GOTO: 5 bytes ---
    else if (kstrcmp(cmd, "GOTO") == 0) {
        if (pass == 2) {
            char name[32];
            get_token(ptr, name);
            uint32_t target = 0xFFFFFFFF;
            for(int i = 0; i < label_count; i++) {
                if(kstrcmp(label_table[i].name, name) == 0) {
                    target = label_table[i].address;
                    break;
                }
            }
            out_buf[*pos] = 0xE9; 
            int32_t offset = (int32_t)target - (int32_t)(*pos + 5);
            kmemcpy(&out_buf[*pos + 1], &offset, 4);
        }
        *pos += 5;
    }

    // --- SET: Generates "MOV [addr], imm32" (10 bytes) ---
    else if (kstrcmp(cmd, "SET") == 0) {
        if (pass == 2) {
            char var_name[32], val_str[32];
            ptr = get_token(ptr, var_name);
            ptr = get_token(ptr, val_str);
            uint32_t offset = get_var_offset(var_name);
            uint32_t val = (val_str[0] == '0' && val_str[1] == 'x') ? katoh(val_str) : katoi(val_str);

            out_buf[(*pos)++] = 0xC7; // MOV DWORD PTR [mem]
            out_buf[(*pos)++] = 0x05; // ModR/M for disp32
            kmemcpy(&out_buf[*pos], &offset, 4); *pos += 4;
            kmemcpy(&out_buf[*pos], &val, 4);    *pos += 4;
        } else {
            *pos += 10;
        }
    }

    // --- ADD: Generates "ADD [addr], imm32" (10 bytes) ---
    else if (kstrcmp(cmd, "ADD") == 0) {
        if (pass == 2) {
            char var_name[32], val_str[32];
            ptr = get_token(ptr, var_name);
            ptr = get_token(ptr, val_str);
            uint32_t offset = get_var_offset(var_name);
            uint32_t amount = katoi(val_str);

            out_buf[(*pos)++] = 0x81; // ADD DWORD PTR [mem]
            out_buf[(*pos)++] = 0x05; 
            kmemcpy(&out_buf[*pos], &offset, 4); *pos += 4;
            kmemcpy(&out_buf[*pos], &amount, 4); *pos += 4;
        } else {
            *pos += 10;
        }
    }

    else if (kstrcmp(cmd, "PRINT") == 0) {
    const char* p = line;
    while (*p == ' ' || *p == '\t') p++; // Skip whitespace
    p += 5; // Skip "PRINT"
    while (*p == ' ' || *p == '\t') p++; // Skip whitespace after command

    if (*p == '\"') {
        // --- EXISTING STRING LOGIC ---
        char str_val[128];
        p++; // skip quote
        uint32_t s_len = 0;
        while (*p != '\"' && *p != '\0' && s_len < 127) str_val[s_len++] = *p++;
        str_val[s_len++] = '\0';
        if (*p == '\"') p++;

        if (pass == 2) {
            char t1[32], t2[32], t3[32];
            p = get_token(p, t1); p = get_token(p, t2); p = get_token(p, t3);

            out_buf[(*pos)++] = 0xEB; out_buf[(*pos)++] = (uint8_t)s_len;
            uint32_t str_offset = *pos;
            for(uint32_t j = 0; j < s_len; j++) out_buf[(*pos)++] = (uint8_t)str_val[j];

            emit_load(0xB8, "7", out_buf, pos);        // Syscall 7 (Print String)
            emit_mov(0xBB, str_offset, out_buf, pos);
            emit_load(0xB9, t1, out_buf, pos);         // X
            emit_load(0xBA, t2, out_buf, pos);         // Y
            emit_load(0xBF, t3, out_buf, pos);         // Color
            out_buf[(*pos)++] = 0xCD; out_buf[(*pos)++] = 0x80;
        } else {
            *pos += (2 + s_len + 30 + 2);
        }
    } 
    else {
        // --- NEW VARIABLE/NUMBER LOGIC ---
        char var_name[32], t1[32], t2[32], t3[32];
        p = get_token(p, var_name);
        p = get_token(p, t1); // x
        p = get_token(p, t2); // y
        p = get_token(p, t3); // color

        if (pass == 2) {
            emit_load(0xB8, "9", out_buf, pos);        // Syscall 9 (Print Number)
            emit_load(0xBB, var_name, out_buf, pos);   // The Value
            emit_load(0xB9, t1, out_buf, pos);         // X
            emit_load(0xBA, t2, out_buf, pos);         // Y
            emit_load(0xBF, t3, out_buf, pos);         // Color
            out_buf[(*pos)++] = 0xCD; out_buf[(*pos)++] = 0x80;
        } else {
            *pos += 32; // 5 loads (30) + INT (2)
        }
    }
}

    else if (kstrcmp(cmd, "SLEEP") == 0) {
        if (pass == 2) {
            ptr = get_token(ptr, arg_str); // Fixed: arg_str is now declared
            emit_load(0xB8, "3", out_buf, pos);
            emit_load(0xBB, arg_str, out_buf, pos);
            out_buf[(*pos)++] = 0xCD; out_buf[(*pos)++] = 0x80;
        } else {
            *pos += 14;
        }
    }    // --- RECT: 6 Loads (36) + INT (2) = 38 bytes ---
    else if (kstrcmp(cmd, "RECT") == 0) {
        if (pass == 2) {
            char t1[32], t2[32], t3[32], t4[32], t5[32];
            ptr = get_token(ptr, t1); ptr = get_token(ptr, t2);
            ptr = get_token(ptr, t3); ptr = get_token(ptr, t4);
            ptr = get_token(ptr, t5);

            emit_load(0xB8, "6", out_buf, pos); // Syscall 6
            emit_load(0xBB, t1, out_buf, pos);  // X
            emit_load(0xB9, t2, out_buf, pos);  // Y
            emit_load(0xBA, t3, out_buf, pos);  // W
            emit_load(0xBE, t4, out_buf, pos);  // H
            emit_load(0xBF, t5, out_buf, pos);  // Color
            out_buf[(*pos)++] = 0xCD; out_buf[(*pos)++] = 0x80;
        } else {
            *pos += 38;
        }
    }

    

    else if (kstrcmp(cmd, "CLEAR") == 0 || kstrcmp(cmd, "EXIT") == 0) {
        if (pass == 2) {
            const char* id = (kstrcmp(cmd, "CLEAR") == 0) ? "5" : "4";
            emit_load(0xB8, id, out_buf, pos);
            out_buf[(*pos)++] = 0xCD; out_buf[(*pos)++] = 0x80;
        } else {
            *pos += 8;
        }
    }
else if (kstrcmp(cmd, "CMP") == 0) {
    char var_name[32], val_str[32];
    ptr = get_token(ptr, var_name);
    ptr = get_token(ptr, val_str);
    
    if (pass == 2) {
        // Load the variable into EAX to compare it
        emit_load(0xB8, var_name, out_buf, pos);
        
        // CMP EAX, immediate_value (Opcode 0x3D)
        uint32_t val = (val_str[0] == '0' && val_str[1] == 'x') ? katoh(val_str) : katoi(val_str);
        out_buf[(*pos)++] = 0x3D; 
        kmemcpy(&out_buf[*pos], &val, 4);
        *pos += 4;
    } else {
        *pos += 11; // 6 (emit_load) + 5 (cmp)
    }
}
else if (kstrcmp(cmd, "JE") == 0 || kstrcmp(cmd, "JL") == 0 || kstrcmp(cmd, "JG") == 0) {
    char label_name[32];
    get_token(ptr, label_name);

    // Select opcode based on command
    uint8_t condition_byte = 0;
    if (cmd[1] == 'E')      condition_byte = 0x84; // JE (Jump Equal)
    else if (cmd[1] == 'L') condition_byte = 0x8C; // JL (Jump Less)
    else if (cmd[1] == 'G') condition_byte = 0x8F; // JG (Jump Greater)

    if (pass == 2) {
        uint32_t target = 0xFFFFFFFF;
        for(int i = 0; i < label_count; i++) {
            if(kstrcmp(label_table[i].name, label_name) == 0) {
                target = label_table[i].address;
                break;
            }
        }
        
        out_buf[(*pos)++] = 0x0F;            // Prefix
        out_buf[(*pos)++] = condition_byte;   // Opcode
        
        // Offset = Target - (Current_Pos + 4 bytes for the offset itself)
        int32_t offset = (int32_t)target - (int32_t)(*pos + 4);
        kmemcpy(&out_buf[*pos], &offset, 4);
        *pos += 4;
    } else {
        *pos += 6; // 2 bytes for opcode + 4 for offset
    }
}
}
