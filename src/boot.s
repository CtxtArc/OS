section .multiboot
align 4
    dd 0x1BADB002             ; magic
    dd 0x00000005             ; flags (Align modules + Graphics)
    dd -(0x1BADB002 + 0x00000005)
    ; Graphics parameters
    dd 0, 0, 0, 0, 0
    dd 0                      ; 0 = Linear Framebuffer
    dd 1024                   ; Width
    dd 768                    ; Height
    dd 32                     ; BPP

section .bss
align 16
stack_bottom:
    resb 32768                ; Increased to 32 KiB for safety
stack_top:

section .text
global _start:function (_start.end - _start)
_start:
    ; --- STEP 1: ZERO BSS FIRST ---
    ; We do this before setting up the stack so we don't erase our own stack.
    extern __bss_start
    extern _end
    
    mov edi, __bss_start      ; Start of BSS
    mov ecx, _end             ; End of BSS
    sub ecx, edi              ; Size of BSS
    xor eax, eax              ; Value 0
    rep stosb                 ; Fill with zeros

    ; --- STEP 2: SETUP STACK ---
    ; Now that memory is clean and won't be wiped, it is safe to use the stack.
    mov esp, stack_top

    ; --- STEP 3: CALL KERNEL ---
    ; Note: EAX and EBX still contain Multiboot magic/info from the bootloader.
    push ebx                  ; Multiboot info pointer
    push eax                  ; Multiboot magic
    
    extern kmain
    call kmain

    cli
.hang:
    hlt
    jmp .hang
.end:
