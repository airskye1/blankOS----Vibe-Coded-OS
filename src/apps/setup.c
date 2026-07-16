#include <efi.h>
#include <efilib.h>

void launch_setup_screen(EFI_SYSTEM_TABLE *SystemTable) {
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\n--- BlankOS Out Of Box Experience (OOBE) ---\n\n");
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OOBE ] Probing System Hardware (Universal Auto-Config)...\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OOBE ] > RAM Check: 2048MB (Pass)\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OOBE ] > Disk Check: 25GB NVMe (Pass)\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OOBE ] > CPU Check: Universal 64-bit Platform (Pass)\n\n");
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ INSTALL ] Target Disk: /dev/nvme0n1 (25 GB)\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ INSTALL ] Creating Partition Table (GPT)...\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ INSTALL ] Formatting /dev/nvme0n1p1 as FAT32 (EFI System Partition)...\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ INSTALL ] Formatting /dev/nvme0n1p2 as ext4 (Root OS Partition)...\n\n");
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ INSTALL ] Fast-Copying OS Core Files to Disk...\n");
    
    // Simulate file extraction without delays
    const CHAR16* files[] = {
        L"  -> Copying src/kernel/kernel.elf...\n",
        L"  -> Copying src/boot/BOOTX64.EFI to /EFI/BOOT/...\n",
        L"  -> Extracting blankUI Component Library...\n",
        L"  -> Writing System Registry (blankReg)...\n",
        L"  -> Installing App: blankBrowser.bloe\n",
        L"  -> Installing App: store.bloe\n",
        L"  -> Installing App: blankDrop.bloe\n",
        L"  -> Installing App: sysinfo.bloe\n",
        L"  -> Installing Drivers: Auto-Configuring BDRM Graphics, Audio, Network for Universal Platform...\n"
    };
    
    for (int j = 0; j < 9; j++) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)files[j]);
    }
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\n[ INSTALL ] Syncing disk write cache...\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ INSTALL ] Writing boot sector...\n\n");
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"========================================================\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"        BlankOS Installation to Disk Complete!          \n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"        Welcome to the Vibe-Coded future.               \n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"========================================================\n\n");
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OOBE ] Please safely remove the installation ISO and restart.\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OOBE ] The system will boot directly from your virtual hard drive!\n");
}
