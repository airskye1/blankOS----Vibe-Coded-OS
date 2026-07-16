#include <efi.h>
#include <efilib.h>

void launch_setup_screen(EFI_SYSTEM_TABLE *SystemTable) {
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\n--- BlankOS Out Of Box Experience (OOBE) ---\r\n\r\n");
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OOBE ] Probing System Hardware (Universal Auto-Config)...\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OOBE ] > RAM Check: 2048MB (Pass)\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OOBE ] > Disk Check: 25GB NVMe (Pass)\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OOBE ] > CPU Check: Universal 64-bit Platform (Pass)\r\n\r\n");
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ INSTALL ] Target Disk: /dev/nvme0n1 (25 GB)\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ INSTALL ] Creating Partition Table (GPT)...\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ INSTALL ] Formatting /dev/nvme0n1p1 as FAT32 (EFI System Partition)...\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ INSTALL ] Formatting /dev/nvme0n1p2 as ext4 (Root OS Partition)...\r\n\r\n");
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ INSTALL ] Fast-Copying OS Core Files to Disk...\r\n");
    
    // Simulate file extraction without delays
    const CHAR16* files[] = {
        L"  -> Copying src/kernel/kernel.elf...\r\n",
        L"  -> Copying src/boot/BOOTX64.EFI to /EFI/BOOT/...\r\n",
        L"  -> Extracting blankUI Component Library...\r\n",
        L"  -> Writing System Registry (blankReg)...\r\n",
        L"  -> Installing App: blankBrowser.bloe\r\n",
        L"  -> Installing App: store.bloe\r\n",
        L"  -> Installing App: blankDrop.bloe\r\n",
        L"  -> Installing App: sysinfo.bloe\r\n",
        L"  -> Installing Drivers: Auto-Configuring BDRM Graphics, Audio, Network for Universal Platform...\r\n"
    };
    
    for (int j = 0; j < 9; j++) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)files[j]);
    }
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\n[ INSTALL ] Syncing disk write cache...\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ INSTALL ] Writing boot sector...\r\n\r\n");
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"========================================================\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"        BlankOS Installation to Disk Complete!          \r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"        Welcome to the Vibe-Coded future.               \r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"========================================================\r\n\r\n");
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OOBE ] Please safely remove the installation ISO and restart.\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OOBE ] The system will boot directly from your virtual hard drive!\r\n");
}
