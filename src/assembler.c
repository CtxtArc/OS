#include "assembler.h"
#include "lib.h"
#include "idt.h"

void assemble_line(const char* line, uint8_t* out_buf, uint32_t* pos) {
    char cmd[32];
    char arg_str[32];
    
    // Get the command name
    const char* ptr = get_token(line, cmd);
    if (!ptr) return;

    if (kstrcmp(cmd, "DRAW_CHAR") == 0) {
        // Syntax: DRAW_CHAR <ascii> <x> <y>
        uint32_t char_val, x, y;
        
        ptr = get_token(ptr, arg_str);
        char_val = katoi(arg_str);
        
        ptr = get_token(ptr, arg_str);
        x = katoi(arg_str);
        
        ptr = get_token(ptr, arg_str);
        y = katoi(arg_str);

        emit_mov(0xB8, 1, out_buf, pos);    // MOV EAX, 1
        emit_mov(0xBB, char_val, out_buf, pos); // MOV EBX, char
        emit_mov(0xB9, x, out_buf, pos);    // MOV ECX, x
        emit_mov(0xBA, y, out_buf, pos);    // MOV EDX, y
        
        out_buf[(*pos)++] = 0xCD; // INT 0x80
        out_buf[(*pos)++] = 0x80;
    }
    else if (kstrcmp(cmd, "GET_TICKS") == 0) {
        emit_mov(0xB8, 2, out_buf, pos); // MOV EAX, 2
        out_buf[(*pos)++] = 0xCD; 
        out_buf[(*pos)++] = 0x80;
    }
    else if (kstrcmp(cmd, "SLEEP") == 0) {
        uint32_t ms;
        ptr = get_token(ptr, arg_str);
        ms = katoi(arg_str);

        emit_mov(0xB8, 3, out_buf, pos); // EAX=3
        emit_mov(0xBB, ms, out_buf, pos); // EBX=ms
        out_buf[(*pos)++] = 0xCD; 
        out_buf[(*pos)++] = 0x80;
    }
    else if (kstrcmp(cmd, "EXIT") == 0) {
        emit_mov(0xB8, 4, out_buf, pos); // EAX=4
        out_buf[(*pos)++] = 0xCD; 
        out_buf[(*pos)++] = 0x80;
    }
    else if (kstrcmp(cmd, "NOP") == 0) {
        out_buf[(*pos)++] = 0x90;
    }
    else if (kstrcmp(cmd, "HLT") == 0) {
        out_buf[(*pos)++] = 0xF4;
    }
else if (kstrcmp(cmd, "RECT") == 0) {
    // Syntax: RECT x y w h color
    char arg_str[32];
    uint32_t x, y, w, h, color;

    ptr = get_token(ptr, arg_str); x = katoi(arg_str);
    ptr = get_token(ptr, arg_str); y = katoi(arg_str);
    ptr = get_token(ptr, arg_str); w = katoi(arg_str);
    ptr = get_token(ptr, arg_str); h = katoi(arg_str);
    ptr = get_token(ptr, arg_str); color = katoi(arg_str);

    emit_mov(0xB8, 6, out_buf, pos);     // EAX = 6
    emit_mov(0xBB, x, out_buf, pos);     // EBX = x
    emit_mov(0xB9, y, out_buf, pos);     // ECX = y
    emit_mov(0xBA, w, out_buf, pos);     // EDX = w
    emit_mov(0xBE, h, out_buf, pos);     // ESI = h (Opcode 0xBE)
    emit_mov(0xBF, color, out_buf, pos); // EDI = color (Opcode 0xBF)
    
    out_buf[(*pos)++] = 0xCD; // INT 0x80
    out_buf[(*pos)++] = 0x80;
}
else if (kstrcmp(cmd, "CLEAR") == 0) {
    // We only need to set EAX to 5 and trigger the interrupt
    emit_mov(0xB8, 5, out_buf, pos); // MOV EAX, 5
    out_buf[(*pos)++] = 0xCD;        // INT 0x80
    out_buf[(*pos)++] = 0x80;
}
else if (kstrcmp(cmd, "PRINT") == 0) {
    char str_val[128];
    char tmp[32];
    uint32_t x, y, color;

    // 1. Extract the string between quotes
    const char* p = line;
    while (*p != '\"' && *p != '\0') p++;
    int s_idx = 0;
    if (*p == '\"') {
        p++;
        while (*p != '\"' && *p != '\0' && s_idx < 127) str_val[s_idx++] = *p++;
        str_val[s_idx++] = '\0'; // Include null terminator
        if (*p == '\"') p++;
    }

    // 2. Extract X, Y, and Color
    p = get_token(p, tmp); x = katoi(tmp);
    p = get_token(p, tmp); y = katoi(tmp);
    p = get_token(p, tmp); 
    color = (tmp[0] == '0' && tmp[1] == 'x') ? katoh(tmp) : katoi(tmp);

    // 3. Emit JMP over the string data (2 bytes: EB + length)
    out_buf[(*pos)++] = 0xEB; 
    out_buf[(*pos)++] = (uint8_t)s_idx;

    // 4. Write the String Data into the binary
    uint32_t str_offset = *pos;
    for (int j = 0; j < s_idx; j++) {
        out_buf[(*pos)++] = (uint8_t)str_val[j];
    }

    // 5. Emit Syscall Setup
    emit_mov(0xB8, 7, out_buf, pos);          // EAX = 7
    emit_mov(0xBB, str_offset, out_buf, pos); // EBX = Offset to string
    emit_mov(0xB9, x, out_buf, pos);          // ECX = X
    emit_mov(0xBA, y, out_buf, pos);          // EDX = Y
    emit_mov(0xBF, color, out_buf, pos);      // EDI = Color
    
    out_buf[(*pos)++] = 0xCD; // INT 0x80
    out_buf[(*pos)++] = 0x80;
}
}


