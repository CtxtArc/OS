global gdt_flush
gdt_flush:
    mov eax, [esp + 4]  ; Get the pointer to the GDT descriptor
    lgdt [eax]          ; Load the GDT

    mov ax, 0x10        ; 0x10 is our Data Segment (entry 2)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:.flush     ; 0x08 is our Code Segment (entry 1). Far jump!
.flush:
    ret

global tss_flush
tss_flush:
    mov ax, 0x2B ; 0x28 is the 5th entry in GDT, OR'd with RPL 3 (0x28 | 3 = 0x2B)
    ltr ax
    ret


