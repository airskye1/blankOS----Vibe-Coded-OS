[bits 64]

extern kernel_main

global kernel_entry

section .text

; void kernel_entry(EFI_SYSTEM_TABLE *SystemTable);
; The GNU-EFI crt0 correctly passed the SystemTable to boot.c via System V ABI.
; boot.c passes it to us here in RDI.
kernel_entry:
    ; 1. Set up a robust, guaranteed 16-byte aligned hardware stack frame.
    ; This explicitly prevents the GCC Microsoft ABI stack corruption bugs!
    push rbp
    mov rbp, rsp
    and rsp, -16      ; Force 16-byte alignment for SSE/Firmware safety
    
    ; 2. SystemTable is already perfectly sitting in RDI. 
    ; Hand off execution to the C++ Monolithic OS!
    call kernel_main

    ; 3. If the C++ OS ever unexpectedly returns, securely halt the CPU.
    mov rsp, rbp
    pop rbp
.halt:
    cli
    hlt
    jmp .halt
