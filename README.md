# BlankOS

BlankOS is a modern, highly aesthetic, fully custom operating system built from scratch. It features a custom UEFI bootloader, a proprietary Direct Rendering Manager (BDRM) with HDR and VRR support, the native `blankUI` component library, and its own proprietary `.bloe` executable format.

## Disclaimer
> **This is a vibe coded OS, and it was "overseen" by airskye.**

## Features
- **Custom Kernel**: Independent from Linux, featuring preemptive multitasking and a secure memory manager.
- **blankUI**: A beautiful native component toolkit featuring modals, frosted glass, drop shadows, and built-in animations.
- **Security**: Features a secure login screen utilizing kernel-level SHA-256 password hashing.
- **blankReg**: A hierarchical configuration database for managing system and user settings.
- **Advanced Graphics**: BDRM architecture designed for high dynamic range and variable refresh rates.

## Downloading and Installation
This operating system uses ELF64 and EFI formats. To get the OS running:

1. Navigate to the **Releases** tab on the right side of this GitHub repository.
2. Download the latest compiled `blankOS.iso` file attached to the release.
3. Load the ISO into an emulator like QEMU or VirtualBox.
4. **Crucial:** You must enable **EFI / UEFI** in your virtual machine settings for the bootloader to execute.

## Compilation for Developers
If you wish to compile the operating system yourself without a local toolchain:

1. Push your code to the `main` branch.
2. The included GitHub Actions workflow will automatically compile the OS, parse `version.json`, and publish a brand new GitHub Release for you!

Alternatively, compile it in a Linux environment using:
```bash
make iso
```

## Running
Load the compiled `blankOS.iso` into an emulator like QEMU or VirtualBox. 
**Crucial:** You must enable **EFI / UEFI** in your virtual machine settings for the bootloader to execute.
