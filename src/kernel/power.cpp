#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <efi.h>

extern "C" {
    __attribute__((weak)) EFI_SYSTEM_TABLE *gSystemTable = NULL;
}

static inline void outb_port(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb_port(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw_port(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw_port(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void do_os_shutdown(void) {
    // 1. UEFI RuntimeServices ResetSystem(EfiResetShutdown)
    if (gSystemTable && gSystemTable->RuntimeServices && gSystemTable->RuntimeServices->ResetSystem) {
        gSystemTable->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    }

    // 2. Hardware ACPI PM1a_CNT S5 Soft-Off Ports (QEMU ICH9 0x604, PIIX4/Bochs 0xB004, VBox 0x4004)
    outw_port(0x604, 0x2000);
    outw_port(0xB004, 0x2000);
    outw_port(0x4004, 0x3400);
    outb_port(0xF4, 0x00);

    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

static void do_os_restart(void) {
    // 1. UEFI RuntimeServices ResetSystem(EfiResetCold)
    if (gSystemTable && gSystemTable->RuntimeServices && gSystemTable->RuntimeServices->ResetSystem) {
        gSystemTable->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
    }

    // 2. 8042 PS/2 Controller CPU Reset Line Pulse (0x64 <- 0xFE)
    int wait_loops = 10000;
    while ((inb_port(0x64) & 0x02) != 0 && --wait_loops > 0) {}
    outb_port(0x64, 0xFE);

    // 3. PCI Reset Control Register (0xCF9: SYS_RST | RST_CPU)
    uint8_t cf9 = inb_port(0xCF9) & ~0x06;
    outb_port(0xCF9, (uint8_t)(cf9 | 0x02));
    outb_port(0xCF9, (uint8_t)(cf9 | 0x06));

    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

static void do_os_sleep(void) {
    // Write CMOS Shutdown Status 0x0F = 0x05 (warm resume) and enter ACPI S1/C1 CPU sleep until IRQ wakeup
    outb_port(0x70, 0x0F);
    outb_port(0x71, 0x05);
    __asm__ volatile("sti; hlt");
}

static void do_os_hibernate(void) {
    // Flush caches (WBINVD) and enter deep sleep state
    __asm__ volatile("wbinvd");
    __asm__ volatile("sti; hlt");
}

void os_shutdown(void) {
    do_os_shutdown();
}

void os_restart(void) {
    do_os_restart();
}

void os_sleep(void) {
    do_os_sleep();
}

void os_hibernate(void) {
    do_os_hibernate();
}

extern "C" {
    void os_shutdown_c(void) __asm__("os_shutdown");
    void os_shutdown_c(void) {
        do_os_shutdown();
    }

    void os_restart_c(void) __asm__("os_restart");
    void os_restart_c(void) {
        do_os_restart();
    }

    void os_sleep_c(void) __asm__("os_sleep");
    void os_sleep_c(void) {
        do_os_sleep();
    }

    void os_hibernate_c(void) __asm__("os_hibernate");
    void os_hibernate_c(void) {
        do_os_hibernate();
    }
}
