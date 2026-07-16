#include <stdint.h>

// I/O Port operations
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// CMOS RTC Ports
#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71

// Read a register from the Real Time Clock
uint8_t rtc_read_register(int reg) {
    outb(CMOS_ADDRESS, reg);
    return inb(CMOS_DATA);
}

// A simple function to get the current system time in seconds
// In a real OS, this would parse the BCD (Binary Coded Decimal) values from the RTC
uint64_t get_system_time(void) {
    // Read Seconds, Minutes, Hours, Day, Month, Year from RTC
    // Convert from BCD to binary
    // Calculate UNIX timestamp
    return 0; // Stub
}
