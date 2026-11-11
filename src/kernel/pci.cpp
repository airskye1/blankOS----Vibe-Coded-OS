#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <efi.h>

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t header_type;
    uint8_t irq_line;
    uint32_t bar0;
    uint32_t bars[6];
    const char* vendor_name;
    const char* device_name;
    const char* driver_name;
} PciDeviceInfo;

#define MAX_PCI_DEVICES 64

static PciDeviceInfo g_pci_devices[MAX_PCI_DEVICES];
static int g_pci_device_count = 0;
static bool g_pci_enumerated = false;

static char g_cpu_vendor[16] = {0};
static char g_cpu_brand[64] = {0};
static bool g_cpu_probed = false;

static inline void cpuid_exec(uint32_t leaf, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx) {
    __asm__ volatile(
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(0)
    );
}

static void probe_cpu_hardware(void) {
    if (g_cpu_probed) return;
    g_cpu_probed = true;

    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    cpuid_exec(0x00000000, &eax, &ebx, &ecx, &edx);

    ((uint32_t*)g_cpu_vendor)[0] = ebx;
    ((uint32_t*)g_cpu_vendor)[1] = edx;
    ((uint32_t*)g_cpu_vendor)[2] = ecx;
    g_cpu_vendor[12] = '\0';

    uint32_t max_ext = 0;
    cpuid_exec(0x80000000, &max_ext, &ebx, &ecx, &edx);
    if (max_ext >= 0x80000004) {
        uint32_t* brand_words = (uint32_t*)g_cpu_brand;
        cpuid_exec(0x80000002, &brand_words[0], &brand_words[1], &brand_words[2], &brand_words[3]);
        cpuid_exec(0x80000003, &brand_words[4], &brand_words[5], &brand_words[6], &brand_words[7]);
        cpuid_exec(0x80000004, &brand_words[8], &brand_words[9], &brand_words[10], &brand_words[11]);
        g_cpu_brand[48] = '\0';

        // Trim leading spaces in CPU brand string
        int start = 0;
        while (g_cpu_brand[start] == ' ') start++;
        if (start > 0) {
            int i = 0;
            while (g_cpu_brand[start + i] != '\0') {
                g_cpu_brand[i] = g_cpu_brand[start + i];
                i++;
            }
            g_cpu_brand[i] = '\0';
        }
    } else {
        for (int i = 0; i < 13; i++) g_cpu_brand[i] = g_cpu_vendor[i];
    }
}

static const char* pci_lookup_vendor_name(uint16_t vendor_id) {
    switch (vendor_id) {
        case 0x8086: return "Intel Corporation";
        case 0x15AD: return "VMware, Inc.";
        case 0x1234: return "QEMU / Bochs";
        case 0x1AF4: return "Red Hat / VirtIO";
        case 0x10EC: return "Realtek Semiconductor";
        case 0x14E4: return "Broadcom Inc.";
        case 0x1274: return "Ensoniq";
        case 0x1022: return "Advanced Micro Devices (AMD)";
        case 0x10DE: return "NVIDIA Corporation";
        case 0x1B36: return "Red Hat, Inc.";
        case 0x80EE: return "InnoTek / VirtualBox";
        default:     return "PCI Hardware Vendor";
    }
}

static void pci_lookup_device_and_driver(
    uint16_t vendor_id,
    uint16_t device_id,
    uint8_t class_code,
    uint8_t subclass,
    const char** out_device_name,
    const char** out_driver_name
) {
    if (vendor_id == 0x15AD) {
        if (device_id == 0x0405) {
            *out_device_name = "VMware SVGA II Graphics Adapter";
            *out_driver_name = "svga2.sys";
            return;
        }
        if (device_id == 0x1977) {
            *out_device_name = "VMware HD Audio Controller";
            *out_driver_name = "hdaudio.sys";
            return;
        }
        if (device_id == 0x0740) {
            *out_device_name = "VMware VMCI Bus Controller";
            *out_driver_name = "pci_bridge.sys";
            return;
        }
        if (device_id == 0x0790 || device_id == 0x07A0) {
            *out_device_name = "VMware PCI Express Root Bridge";
            *out_driver_name = "pci_bridge.sys";
            return;
        }
        if (device_id == 0x0770 || device_id == 0x0774 || device_id == 0x0778 || device_id == 0x0779) {
            *out_device_name = "VMware USB xHCI/EHCI Host Controller";
            *out_driver_name = "xhci_usb.sys";
            return;
        }
    } else if (vendor_id == 0x1234 && device_id == 0x1111) {
        *out_device_name = "QEMU Standard VGA (Bochs VBE)";
        *out_driver_name = "svga2.sys";
        return;
    } else if (vendor_id == 0x1274 && device_id == 0x1371) {
        *out_device_name = "Ensoniq ES1371 AudioPCI-97";
        *out_driver_name = "hdaudio.sys";
        return;
    } else if (vendor_id == 0x8086) {
        if (device_id == 0x1237) {
            *out_device_name = "Intel 82441FX PMC Host Bridge";
            *out_driver_name = "pci_bridge.sys";
            return;
        }
        if (device_id == 0x29C0) {
            *out_device_name = "Intel 82G33/Q35 Express DRAM Controller";
            *out_driver_name = "pci_bridge.sys";
            return;
        }
        if (device_id == 0x7000 || device_id == 0x2918) {
            *out_device_name = "Intel PIIX3/ICH9LPC ISA Bridge";
            *out_driver_name = "pci_bridge.sys";
            return;
        }
        if (device_id == 0x7010) {
            *out_device_name = "Intel 82371SB PIIX3 IDE Controller";
            *out_driver_name = "ahci_stor.sys";
            return;
        }
        if (device_id == 0x7113 || device_id == 0x2930) {
            *out_device_name = "Intel PIIX4/ICH9 ACPI/SMBus Controller";
            *out_driver_name = "pci_bridge.sys";
            return;
        }
        if (device_id == 0x100E || device_id == 0x10D3 || device_id == 0x10EA) {
            *out_device_name = "Intel PRO/1000 Gigabit Ethernet (e1000)";
            *out_driver_name = "e1000_net.sys";
            return;
        }
        if (device_id == 0x2922 || device_id == 0x2829) {
            *out_device_name = "Intel ICH9R/ICH8M 6-Port SATA AHCI Controller";
            *out_driver_name = "ahci_stor.sys";
            return;
        }
        if (device_id == 0x2668 || device_id == 0x27D8 || device_id == 0x1C20 ||
            device_id == 0x8C20 || device_id == 0x284B || device_id == 0x293E) {
            *out_device_name = "Intel High Definition Audio Controller";
            *out_driver_name = "hdaudio.sys";
            return;
        }
    } else if (vendor_id == 0x10EC) {
        if (device_id == 0x8139 || device_id == 0x8168) {
            *out_device_name = "Realtek RTL8111/8168/8139 Gigabit Ethernet";
            *out_driver_name = "e1000_net.sys";
            return;
        }
    } else if (vendor_id == 0x14E4) {
        *out_device_name = "Broadcom 802.11ac Wireless / Bluetooth Adapter";
        *out_driver_name = "e1000_net.sys";
        return;
    } else if (vendor_id == 0x1AF4) {
        if (class_code == 0x02) {
            *out_device_name = "VirtIO Paravirtualized Network Adapter";
            *out_driver_name = "e1000_net.sys";
            return;
        }
        if (class_code == 0x01) {
            *out_device_name = "VirtIO Block / SCSI Storage Controller";
            *out_driver_name = "ahci_stor.sys";
            return;
        }
        if (class_code == 0x03) {
            *out_device_name = "VirtIO GPU 3D Display Adapter";
            *out_driver_name = "svga2.sys";
            return;
        }
    }

    switch (class_code) {
        case 0x01:
            if (subclass == 0x08) {
                *out_device_name = "NVMe Express Solid State Controller";
                *out_driver_name = "nvme.sys";
            } else if (subclass == 0x06) {
                *out_device_name = "Serial ATA AHCI Storage Controller";
                *out_driver_name = "ahci_stor.sys";
            } else if (subclass == 0x01) {
                *out_device_name = "IDE Dual-Channel Storage Controller";
                *out_driver_name = "ahci_stor.sys";
            } else {
                *out_device_name = "Mass Storage Host Controller";
                *out_driver_name = "ahci_stor.sys";
            }
            break;
        case 0x02:
            if (subclass == 0x80) {
                *out_device_name = "Wireless 802.11 Network Controller";
                *out_driver_name = "e1000_net.sys";
            } else {
                *out_device_name = "Gigabit Ethernet Network Controller";
                *out_driver_name = "e1000_net.sys";
            }
            break;
        case 0x03:
            *out_device_name = "PCI Express Graphics Display Controller";
            *out_driver_name = "svga2.sys";
            break;
        case 0x04:
            *out_device_name = "High Definition Audio Controller";
            *out_driver_name = "hdaudio.sys";
            break;
        case 0x06:
            if (subclass == 0x00) {
                *out_device_name = "PCI Express Host Bridge";
            } else if (subclass == 0x01) {
                *out_device_name = "PCI-to-ISA Legacy Bridge";
            } else if (subclass == 0x04) {
                *out_device_name = "PCI-to-PCI Express Root Port";
            } else {
                *out_device_name = "System Architecture Bridge";
            }
            *out_driver_name = "pci_bridge.sys";
            break;
        case 0x0C:
            if (subclass == 0x03) {
                *out_device_name = "USB 3.0 xHCI Host Controller";
                *out_driver_name = "xhci_usb.sys";
            } else if (subclass == 0x05) {
                *out_device_name = "System Management Bus (SMBus) Controller";
                *out_driver_name = "pci_bridge.sys";
            } else {
                *out_device_name = "High-Speed Serial Bus Controller";
                *out_driver_name = "xhci_usb.sys";
            }
            break;
        default:
            *out_device_name = "PCI System Peripheral";
            *out_driver_name = "pci_bridge.sys";
            break;
    }
}

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
        uint32_t lbus  = (uint32_t)bus;
        uint32_t ldev  = (uint32_t)device;
        uint32_t lfunc = (uint32_t)function;

        uint32_t address = (uint32_t)((lbus << 16) | (ldev << 11) | (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
        outl(0xCF8, address);
        return inl(0xCFC);
    }

    void pci_write_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t val) {
        uint32_t lbus  = (uint32_t)bus;
        uint32_t ldev  = (uint32_t)device;
        uint32_t lfunc = (uint32_t)function;

        uint32_t address = (uint32_t)((lbus << 16) | (ldev << 11) | (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
        outl(0xCF8, address);
        outl(0xCFC, val);
    }

    uint16_t pci_get_vendor(uint8_t bus, uint8_t device, uint8_t function) {
        return (uint16_t)(pci_read_dword(bus, device, function, 0x00) & 0xFFFF);
    }

    uint16_t pci_get_device(uint8_t bus, uint8_t device, uint8_t function) {
        return (uint16_t)((pci_read_dword(bus, device, function, 0x00) >> 16) & 0xFFFF);
    }

    const char* pci_get_cpu_vendor(void) {
        probe_cpu_hardware();
        return g_cpu_vendor;
    }

    const char* pci_get_cpu_brand(void) {
        probe_cpu_hardware();
        return g_cpu_brand;
    }

    extern bool svga_init(uint8_t bus, uint8_t device, uint8_t function);
    extern void audio_set_hda_device(uintptr_t base_addr);

    static void pci_record_function(EFI_SYSTEM_TABLE *SystemTable, uint8_t bus, uint8_t dev, uint8_t func) {
        uint32_t id_reg = pci_read_dword(bus, dev, func, 0x00);
        uint16_t vendor = (uint16_t)(id_reg & 0xFFFF);
        if (vendor == 0xFFFF || vendor == 0x0000) {
            return;
        }
        uint16_t device    = (uint16_t)((id_reg >> 16) & 0xFFFF);
        uint32_t class_reg = pci_read_dword(bus, dev, func, 0x08);
        uint8_t class_code = (uint8_t)((class_reg >> 24) & 0xFF);
        uint8_t subclass   = (uint8_t)((class_reg >> 16) & 0xFF);
        uint8_t prog_if    = (uint8_t)((class_reg >> 8) & 0xFF);
        uint32_t hdr_reg   = pci_read_dword(bus, dev, func, 0x0C);
        uint8_t hdr_type   = (uint8_t)((hdr_reg >> 16) & 0x7F);
        uint32_t intr_reg  = pci_read_dword(bus, dev, func, 0x3C);
        uint8_t irq_line   = (uint8_t)(intr_reg & 0xFF);

        uint32_t bars[6] = {0, 0, 0, 0, 0, 0};
        if (hdr_type == 0x00) {
            for (int b = 0; b < 6; b++) {
                bars[b] = pci_read_dword(bus, dev, func, (uint8_t)(0x10 + b * 4));
            }
        } else if (hdr_type == 0x01) {
            bars[0] = pci_read_dword(bus, dev, func, 0x10);
            bars[1] = pci_read_dword(bus, dev, func, 0x14);
        }

        if (g_pci_device_count < MAX_PCI_DEVICES) {
            PciDeviceInfo* info = &g_pci_devices[g_pci_device_count++];
            info->bus = bus;
            info->device = dev;
            info->function = func;
            info->vendor_id = vendor;
            info->device_id = device;
            info->class_code = class_code;
            info->subclass = subclass;
            info->prog_if = prog_if;
            info->header_type = hdr_type;
            info->irq_line = irq_line;
            info->bar0 = bars[0];
            for (int b = 0; b < 6; b++) info->bars[b] = bars[b];
            info->vendor_name = pci_lookup_vendor_name(vendor);
            pci_lookup_device_and_driver(vendor, device, class_code, subclass, &info->device_name, &info->driver_name);
        }

        if (SystemTable && SystemTable->ConOut) {
            if (vendor == 0x8086 && func == 0) {
                SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ HARDWARE ] Probed Intel PCI Controller\r\n");
            }
            if (vendor == 0x14E4) {
                SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ HARDWARE ] Probed Broadcom Network Controller\r\n");
            }
        }

        if (vendor == 0x15AD && device == 0x0405) {
            if (SystemTable && SystemTable->ConOut) {
                SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ HARDWARE ] Found VMware SVGA II GPU!\r\n");
            }
            if (svga_init(bus, dev, func)) {
                if (SystemTable && SystemTable->ConOut) {
                    SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ GPU      ] Hardware acceleration driver loaded.\r\n");
                }
            }
        }

        if ((vendor == 0x8086 && (device == 0x2668 || device == 0x27D8 || device == 0x1C20 || device == 0x8C20 || device == 0x284B || device == 0x293E)) ||
            (vendor == 0x15AD && device == 0x1977) ||
            (vendor == 0x1274 && device == 0x1371) ||
            (class_code == 0x04 && (subclass == 0x03 || subclass == 0x01))) {
            uintptr_t audio_bar0 = (uintptr_t)(bars[0] & ~0xFULL);
            audio_set_hda_device(audio_bar0);
            if (SystemTable && SystemTable->ConOut) {
                SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ AUDIO    ] High Definition Audio Controller Initialized.\r\n");
            }
        }
    }

    void pci_enumerate(EFI_SYSTEM_TABLE *SystemTable) {
        probe_cpu_hardware();
        if (SystemTable && SystemTable->ConOut) {
            SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ SYSTEM ] Enumerating PCI Configuration Space (0xCF8/0xCFC)...\r\n");
        }
        g_pci_device_count = 0;
        g_pci_enumerated = true;

        for (uint16_t bus = 0; bus < 256; bus++) {
            for (uint8_t dev = 0; dev < 32; dev++) {
                uint16_t vendor = pci_get_vendor((uint8_t)bus, dev, 0);
                if (vendor == 0xFFFF || vendor == 0x0000) {
                    continue;
                }
                pci_record_function(SystemTable, (uint8_t)bus, dev, 0);

                uint32_t hdr_reg = pci_read_dword((uint8_t)bus, dev, 0, 0x0C);
                uint8_t header_type = (uint8_t)((hdr_reg >> 16) & 0xFF);
                if ((header_type & 0x80) != 0) {
                    for (uint8_t func = 1; func < 8; func++) {
                        uint16_t f_vendor = pci_get_vendor((uint8_t)bus, dev, func);
                        if (f_vendor != 0xFFFF && f_vendor != 0x0000) {
                            pci_record_function(SystemTable, (uint8_t)bus, dev, func);
                        }
                    }
                }
            }
        }
    }

    int pci_get_device_count(void) {
        if (!g_pci_enumerated) {
            pci_enumerate(NULL);
        }
        return g_pci_device_count;
    }

    bool pci_get_device_info(
        int index,
        uint8_t* bus,
        uint8_t* dev,
        uint8_t* func,
        uint16_t* vendor,
        uint16_t* device,
        uint8_t* class_code,
        uint8_t* subclass,
        uint32_t* bar0,
        const char** vendor_str,
        const char** dev_str,
        const char** drv_str
    ) {
        if (!g_pci_enumerated) {
            pci_enumerate(NULL);
        }
        if (index < 0 || index >= g_pci_device_count) {
            return false;
        }
        const PciDeviceInfo* info = &g_pci_devices[index];
        if (bus)        *bus        = info->bus;
        if (dev)        *dev        = info->device;
        if (func)       *func       = info->function;
        if (vendor)     *vendor     = info->vendor_id;
        if (device)     *device     = info->device_id;
        if (class_code) *class_code = info->class_code;
        if (subclass)   *subclass   = info->subclass;
        if (bar0)       *bar0       = info->bar0;
        if (vendor_str) *vendor_str = info->vendor_name;
        if (dev_str)    *dev_str    = info->device_name;
        if (drv_str)    *drv_str    = info->driver_name;
        return true;
    }
}
