[bits 16]
[org 0x7c00]

start:
    cli
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    sti

    mov si, msg
print:
    lodsb
    or al, al
    jz halt
    mov ah, 0x0E
    int 0x10
    jmp print

halt:
    cli
    hlt
    jmp halt

msg db "BlankOS requires UEFI! Please enable EFI in VM settings.", 13, 10, 0

times 510-($-$$) db 0
dw 0xAA55
