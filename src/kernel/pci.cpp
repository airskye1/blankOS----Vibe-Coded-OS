#include <stdbool.h>
#include <stdint.h>
#include <efi.h>

// Basic PCI configuration space read using x86 I/O ports 0xCF8 and 0xCFC
extern "C" {
    static inline void outl(uint16_t port, uint32_t val) {
        asm volatile ( "outl %0, %1" : : "a"(val), "Nd"(port) );
    }

    static inline uint32_t inl(uint16_t port) {
        uint32_t ret;
        asm volatile ( "inl %1, %0" : "=a"(ret) : "Nd"(port) );
        return ret;
    }

    uint32_t pci_read_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
        uint32_t address;
        uint32_t lbus  = (uint32_t)bus;
        uint32_t ldev  = (uint32_t)device;
        uint32_t lfunc = (uint32_t)function;

        // Bit 31 is enable bit, 23-16 bus, 15-11 device, 10-8 func, 7-2 register
        address = (uint32_t)((lbus << 16) | (ldev << 11) | (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
        
        outl(0xCF8, address);
        return inl(0xCFC);
    }
    
    uint16_t pci_get_vendor(uint8_t bus, uint8_t device, uint8_t function) {
        return (uint16_t)(pci_read_dword(bus, device, function, 0) & 0xFFFF);
    }
    
    uint16_t pci_get_device(uint8_t bus, uint8_t device, uint8_t function) {
        return (uint16_t)((pci_read_dword(bus, device, function, 0) >> 16) & 0xFFFF);
    }
    
    extern bool svga_init(uint8_t bus, uint8_t device, uint8_t function);

    void pci_enumerate(EFI_SYSTEM_TABLE *SystemTable) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ SYSTEM ] Enumerating PCI Bus for Hardware...\r\n");
        for(uint16_t bus = 0; bus < 256; bus++) {
            for(uint8_t dev = 0; dev < 32; dev++) {
                uint16_t vendor = pci_get_vendor((uint8_t)bus, dev, 0);
                if (vendor != 0xFFFF) { // 0xFFFF means no device
                    uint16_t device = pci_get_device((uint8_t)bus, dev, 0);
                    // Mock printing found hardware
                    if (vendor == 0x8086) { // Intel
                        SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ HARDWARE ] Found Intel Hardware Controller\r\n");
                    }
                    if (vendor == 0x14E4) { // Broadcom
                        SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ HARDWARE ] Found Broadcom Wi-Fi/Bluetooth Controller!\r\n");
                    }
                    if (vendor == 0x15AD && device == 0x0405) {
                        SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ HARDWARE ] Found VMware SVGA II GPU!\r\n");
                        if (svga_init((uint8_t)bus, dev, 0)) {
                            SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ GPU      ] Hardware acceleration driver loaded.\r\n");
                        }
                    }
                }
            }
        }
    }
}
