#include "assembler.h"
#include "lib.h"
#include "idt.h"
#include "kheap.h"



Label label_table[64];
int label_count = 0;

void assemble_line(const char* line, uint8_t* out_buf, uint32_t* pos, int pass) {
    char cmd[32];
    char arg_str[32];
    const char* ptr = get_token(line, cmd);
    if (!ptr) return;

    // --- LABEL: No code generated, just address recording ---
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

    // --- GOTO: 5 Bytes (0xE9 + 4-byte relative offset) ---
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

    // --- PRINT: 2 (JMP) + string_len + 25 (5 MOVs) + 2 (INT) ---
    else if (kstrcmp(cmd, "PRINT") == 0) {
        char str_val[128];
        const char* p = line;
        while (*p != '\"' && *p != '\0') p++;
        uint32_t s_len = 0;
        if (*p == '\"') {
            p++;
            while (*p != '\"' && *p != '\0' && s_len < 127) str_val[s_len++] = *p++;
            str_val[s_len++] = '\0'; // Include null
            if (*p == '\"') p++;
        }

        if (pass == 2) {
            char tmp[32]; uint32_t x, y, col;
            p = get_token(p, tmp); x = katoi(tmp);
            p = get_token(p, tmp); y = katoi(tmp);
            p = get_token(p, tmp); col = (tmp[0] == '0' && tmp[1] == 'x') ? katoh(tmp) : katoi(tmp);

            out_buf[(*pos)++] = 0xEB; // JMP short
            out_buf[(*pos)++] = (uint8_t)s_len;
            uint32_t str_offset = *pos;
            for(uint32_t j = 0; j < s_len; j++) out_buf[(*pos)++] = (uint8_t)str_val[j];

            emit_mov(0xB8, 7, out_buf, pos);          // EAX
            emit_mov(0xBB, str_offset, out_buf, pos); // EBX
            emit_mov(0xB9, x, out_buf, pos);          // ECX
            emit_mov(0xBA, y, out_buf, pos);          // EDX
            emit_mov(0xBF, col, out_buf, pos);         // EDI
            out_buf[(*pos)++] = 0xCD; out_buf[(*pos)++] = 0x80;
        } else {
            *pos += (2 + s_len + 25 + 2);
        }
    }

    // --- RECT: 32 Bytes (6 MOVs + INT) ---
    else if (kstrcmp(cmd, "RECT") == 0) {
        if (pass == 2) {
            uint32_t x, y, w, h, col;
            ptr = get_token(ptr, arg_str); x = katoi(arg_str);
            ptr = get_token(ptr, arg_str); y = katoi(arg_str);
            ptr = get_token(ptr, arg_str); w = katoi(arg_str);
            ptr = get_token(ptr, arg_str); h = katoi(arg_str);
            ptr = get_token(ptr, arg_str); col = (arg_str[0] == '0' && arg_str[1] == 'x') ? katoh(arg_str) : katoi(arg_str);

            emit_mov(0xB8, 6, out_buf, pos);
            emit_mov(0xBB, x, out_buf, pos);
            emit_mov(0xB9, y, out_buf, pos);
            emit_mov(0xBA, w, out_buf, pos);
            emit_mov(0xBE, h, out_buf, pos);
            emit_mov(0xBF, col, out_buf, pos);
            out_buf[(*pos)++] = 0xCD; out_buf[(*pos)++] = 0x80;
        } else {
            *pos += 32;
        }
    }

    // --- SLEEP: 12 Bytes (2 MOVs + INT) ---
    else if (kstrcmp(cmd, "SLEEP") == 0) {
        if (pass == 2) {
            ptr = get_token(ptr, arg_str); uint32_t ms = katoi(arg_str);
            emit_mov(0xB8, 3, out_buf, pos);
            emit_mov(0xBB, ms, out_buf, pos);
            out_buf[(*pos)++] = 0xCD; out_buf[(*pos)++] = 0x80;
        } else {
            *pos += 12;
        }
    }

    // --- CLEAR/EXIT: 7 Bytes (1 MOV + INT) ---
    else if (kstrcmp(cmd, "CLEAR") == 0 || kstrcmp(cmd, "EXIT") == 0) {
        uint32_t id = (kstrcmp(cmd, "CLEAR") == 0) ? 5 : 4;
        if (pass == 2) {
            emit_mov(0xB8, id, out_buf, pos);
            out_buf[(*pos)++] = 0xCD; out_buf[(*pos)++] = 0x80;
        } else {
            *pos += 7;
        }
    }
}
