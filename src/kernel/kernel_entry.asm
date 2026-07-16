[bits 64]
extern kernel_main

global _start

section .text
_start:
    ; The UEFI bootloader passes EFI_SYSTEM_TABLE* in RCX (Microsoft x64 ABI).
    ; We must forward RCX as the first argument to kernel_main so it can call
    ; OutputString. Without this, kernel_main receives a garbage pointer and
    ; faults on its very first firmware call.
    mov rcx, rcx   ; SystemTable* is already in RCX from UEFI firmware - explicit for clarity
    call kernel_main

    ; If kernel_main returns, halt the CPU
.halt:
    cli
    hlt
    jmp .halt
