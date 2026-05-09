; --- External Symbols ---
extern timer_handler
extern keyboard_handler
extern syscall_handler
extern isr_handler
extern next_stack_ptr    ; Defined in idt.c or task.c

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

    ; --- TASK SWITCH LOGIC (Timer) ---
    mov eax, [next_stack_ptr]
    test eax, eax       
    jz .no_switch
    mov esp, eax        
    mov dword [next_stack_ptr], 0 ; Clear the pointer after switch
.no_switch:

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

    ; Immediately switch to the woken task (same as irq0)
    mov eax, [next_stack_ptr]
    test eax, eax
    jz .no_switch
    mov esp, eax
    mov dword [next_stack_ptr], 0
.no_switch:
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
    
    ; THE FIX: Removed the destructive 'mov [esp + 32], eax' line here!

    add esp, 4          
    
    ; --- TASK SWITCH LOGIC (Syscall/Sleep) ---
    ; This fixes the GPF 13 by swapping stacks here instead of inside C
    mov eax, [next_stack_ptr]
    test eax, eax
    jz .no_switch_syscall
    mov esp, eax        ; Load the stack of the NEXT task
    mov dword [next_stack_ptr], 0 ; Clear pointer
.no_switch_syscall:

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

global switch_to_stack
switch_to_stack:
    push ebp
    push edi
    push esi
    push ebx
    mov eax, [esp + 20] 
    mov ecx, [esp + 24] 
    mov [eax], esp      
    mov esp, ecx        
    pop ebx
    pop esi
    pop edi
    pop ebp
    ret
