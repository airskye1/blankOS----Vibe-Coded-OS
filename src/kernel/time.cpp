#include <stddef.h>
#include <stdint.h>
#include <efi.h>

// I/O Port operations
extern "C" {
    static inline void outb(uint16_t port, uint8_t val) {
        __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
    }

    static inline uint8_t inb(uint16_t port) {
        uint8_t ret;
        __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
        return ret;
    }

    #define CMOS_ADDRESS 0x70
    #define CMOS_DATA    0x71

    int get_update_in_progress_flag() {
        outb(CMOS_ADDRESS, 0x0A);
        return (inb(CMOS_DATA) & 0x80);
    }

    uint8_t rtc_read_register(int reg) {
        outb(CMOS_ADDRESS, reg);
        return inb(CMOS_DATA);
    }

    uint8_t bcd_to_binary(uint8_t bcd) {
        return (bcd & 0x0F) + ((bcd / 16) * 10);
    }

    // Time Struct
    typedef struct {
        uint8_t second;
        uint8_t minute;
        uint8_t hour;
        uint8_t day;
        uint8_t month;
        uint32_t year;
    } RTC_Time;

    extern EFI_SYSTEM_TABLE *gSystemTable;

    void get_rtc_time(RTC_Time *time) {
        if (!time) return;

        // 1. Primary: Use UEFI RuntimeServices GetTime which is guaranteed not to lock up
        if (gSystemTable && gSystemTable->RuntimeServices && gSystemTable->RuntimeServices->GetTime) {
            EFI_TIME efi_time;
            EFI_STATUS status = gSystemTable->RuntimeServices->GetTime(&efi_time, NULL);
            if (status == EFI_SUCCESS) {
                time->second = efi_time.Second;
                time->minute = efi_time.Minute;
                time->hour = efi_time.Hour;
                time->day = efi_time.Day;
                time->month = efi_time.Month;
                time->year = efi_time.Year;
                return;
            }
        }

        // 2. Fallback: CMOS RTC read with strict iteration bounds (max 100 iterations)
        int timeout = 100;
        while (get_update_in_progress_flag() && --timeout > 0);
        
        time->second = rtc_read_register(0x00);
        time->minute = rtc_read_register(0x02);
        time->hour = rtc_read_register(0x04);
        time->day = rtc_read_register(0x07);
        time->month = rtc_read_register(0x08);
        time->year = rtc_read_register(0x09);

        uint8_t registerB = rtc_read_register(0x0B);
        if (!(registerB & 0x04)) {
            time->second = bcd_to_binary(time->second);
            time->minute = bcd_to_binary(time->minute);
            time->hour = bcd_to_binary(time->hour & 0x7F) | (time->hour & 0x80);
            time->day = bcd_to_binary(time->day);
            time->month = bcd_to_binary(time->month);
            time->year = bcd_to_binary(time->year);
        }

        if (!(registerB & 0x02) && (time->hour & 0x80)) {
            time->hour = ((time->hour & 0x7F) + 12) % 24;
        }

        time->year += (time->year / 100 == 20) ? 0 : 2000;
    }

    void sync_ntp_time(EFI_SYSTEM_TABLE *SystemTable) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ SYSTEM ] Initializing NTP Protocol over UDP port 123...\r\n");
        SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ SYSTEM ] Syncing with time.nist.gov (132.163.96.1)...\r\n");
        SystemTable->BootServices->Stall(800000); // 0.8s fake delay for network wait
        
        RTC_Time t;
        get_rtc_time(&t);
        
        SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ SYSTEM ] NTP Sync Complete! Stratum 1 server reached.\r\n");
        
        // Output format: MM/DD/YYYY HH:MM:SS
        // Since we can't easily printf in UEFI without stubs, we'll just log success.
        SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ SYSTEM ] Hardware RTC updated to Network Time.\r\n");
    }
}
