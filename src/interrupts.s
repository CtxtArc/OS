; --- External Symbols ---
extern timer_handler
extern keyboard_handler
extern syscall_handler
extern isr_handler

; --- Macros for Processor Exceptions ---
%macro ISR_NOERRCODE 1
  global isr%1
  isr%1:
    push byte 0
    push byte %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERRCODE 1
  global isr%1
  isr%1:
    push byte %1
    jmp isr_common_stub
%endmacro

; Define the 32 standard CPU exceptions
ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_ERRCODE   21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_ERRCODE   30
ISR_NOERRCODE 31

; --- Exception Stub ---
isr_common_stub:
    pusha                
    mov ax, ds
    push eax             

    mov ax, 0x10         
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp             
    call isr_handler
    
    add esp, 4
    pop eax              
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    popa
    add esp, 8           
    iret

; --- Hardware IRQ Handlers ---

global irq0_handler
irq0_handler:
    push byte 0          
    push byte 32         
    pusha                
    mov ax, ds
    push eax             

    mov ax, 0x10         
    mov ds, ax
    mov es, ax

    push esp             
    call timer_handler
    add esp, 4           

    ; --- UNIFIED CONTEXT SWITCH (Timer) ---
    mov esp, eax         ; Load the stack pointer returned by C

    pop eax              
    mov ds, ax
    mov es, ax
    popa
    add esp, 8           
    iret

global irq1_handler
irq1_handler:
    push byte 0
    push byte 33
    pusha
    mov ax, ds
    push eax
    
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    
    push esp
    call keyboard_handler
    add esp, 4

    ; --- UNIFIED CONTEXT SWITCH (Keyboard Wakeup) ---
    mov esp, eax         ; Load the stack pointer returned by C

    pop eax
    mov ds, ax
    mov es, ax
    popa
    add esp, 8
    iret

; --- System Call Handler (int 0x80) ---

global isr128_stub
isr128_stub:
    push byte 0          
    push dword 128       
    pusha                
    
    mov ax, ds
    push eax             

    mov ax, 0x10         
    mov ds, ax
    mov es, ax

    push esp             
    call syscall_handler
    add esp, 4           
    
    ; --- UNIFIED CONTEXT SWITCH (Syscall/Sleep) ---
    mov esp, eax         ; Load the stack pointer returned by C

    pop eax              
    mov ds, ax
    mov es, ax

    popa                 
    add esp, 8           
    iret

; --- Utility Functions ---

global idt_flush
idt_flush:
    mov eax, [esp + 4]
    lidt [eax]
    ret
