#include <stdint.h>
#include <stdbool.h>

// I/O Port operations (borrowed from time.c/kernel space)
extern void outb(uint16_t port, uint8_t val);

// Advanced Configuration and Power Interface (ACPI) Stubs
// In a real OS, we would parse the ACPI FADT table to find the PM1a_CNT_BLK register to trigger these states.

void os_shutdown(void) {
    // QEMU / Bochs specific shutdown port
    outb(0xF4, 0x00); 
    
    // Fallback ACPI shutdown (S5 state)
    // outb(PM1a_CNT, SLP_EN | SLP_TYPa);
    
    while(1) {
        __asm__ volatile("cli; hlt"); // Halt CPU if shutdown fails
    }
}

void os_restart(void) {
    // Keyboard controller reset (Legacy)
    outb(0x64, 0xFE); 
    
    // Fallback ACPI reset
    // outb(RESET_REG, RESET_VALUE);
    
    while(1) {
        __asm__ volatile("cli; hlt"); 
    }
}

void os_sleep(void) {
    // Suspend to RAM (ACPI S3 state)
    // 1. Save CPU context
    // 2. Put devices in D3 state
    // outb(PM1a_CNT, SLP_EN | S3_TYP);
}

void os_hibernate(void) {
    // Suspend to Disk (ACPI S4 state)
    // 1. Write entire physical RAM contents to the swap partition on the NVMe/SATA drive
    // outb(PM1a_CNT, SLP_EN | S4_TYP);
}
