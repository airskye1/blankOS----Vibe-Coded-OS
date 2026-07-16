[bits 64]
extern kernel_main

global _start

section .text
_start:
    ; The UEFI bootloader has already set up 64-bit mode and basic paging.
    ; We just need to call the C kernel entry point.
    call kernel_main

    ; If kernel_main returns, halt the CPU
.halt:
    cli
    hlt
    jmp .halt
