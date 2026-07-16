# BlankOS - Vibe-Coded OS Session Run-Through

This document serves as a comprehensive summary of our development session building **BlankOS**, a custom monolithic vibe-coded operating system. 

We faced several intense low-level architectural challenges and successfully solved every single one to create a robust, ultra-fast booting UEFI operating system with automated CI/CD!

## 1. The Missing C Headers & The Auto-Linter
**The Issue:** Early in the session, compilation was failing because standard C types (`bool`, `uint32_t`, `NULL`) were being used without their required header includes (`<stdbool.h>`, `<stdint.h>`, `<stddef.h>`).
**The Fix:** We created a custom PowerShell script (`linter.ps1`) dubbed the "Vibe-Coded Auto-Linter". This script automatically scans the entire `.c` and `.h` codebase and injects the missing headers wherever it detects the keywords, saving us from manually fixing dozens of files.

## 2. Automated GitHub Release Pipeline
**The Issue:** Manually compiling the ELF64 ISO and uploading it for every test was incredibly tedious.
**The Fix:** We built a highly robust GitHub Actions workflow (`.github/workflows/build.yml`). Upon every push, the workflow installs all dependencies (`mtools`, `gnu-efi`, `nasm`, etc.), compiles the ISO, parses the `version.json` using `jq`, and automatically publishes a beautiful, tagged GitHub Release containing the latest changelogs and the bootable `blankOS.iso`!

## 3. The Visual Out-Of-Box Experience (OOBE)
**The Issue:** We wanted the OS to actually feel like it was installing to the hard drive in VirtualBox.
**The Fix:** We built a highly detailed text-mode installation sequence in `setup.c`. It visually probes the hardware, detects the `/dev/nvme0n1` virtual disk, formats a GPT partition table, and prints out a detailed log of it extracting the `.bloe` apps and `blankUI` components! 

## 4. The "Loading Forever" Delay Bug
**The Issue:** To make the OOBE installer look cool, we added massive `volatile` delay loops. However, when run in an unaccelerated emulator (like VirtualBox without Hardware Virtualization), those loops took 5+ minutes to finish, making it seem like the OS was frozen forever!
**The Fix:** We ripped out the delay loops entirely and converted the installer to a "Universal Auto-Config" profile. The OS now posts and dumps its entire sequence to the screen in less than a second!

## 5. The Silent VirtualBox Crash
**The Issue:** The most complex bug of the session. VirtualBox would find the bootloader, start it, and then instantly and silently freeze without printing a single word to the screen.
**The Fixes:** This was a multi-layered architectural nightmare that we successfully solved:
* **The Missing `.rodata` and `.bss`**: The `Makefile` was using `objcopy` to convert the ELF object into a PE/COFF `.EFI` application, but it forgot to copy the `.rodata` section! All of our text strings were being stripped out. When the OS tried to print them, it crashed! We explicitly added `-j .rodata` and `-j .bss`.
* **The Variadic `Print` Bug**: GNU-EFI's variadic `Print()` function corrupts CPU registers in VirtualBox due to ABI mismatches between GCC (System V) and Microsoft's UEFI ABI. We rewrote the bootloader to entirely bypass `Print()` and use direct firmware pointers: `SystemTable->ConOut->OutputString()`.
* **Strict UEFI CRLF**: Standard C uses `\n` for newlines. UEFI strictly requires `\r\n` (Carriage Return + Line Feed). We updated every single string in the OS to use `\r\n`.
* **ABI Mismatch & Stack Corruption**: We accidentally added `EFIAPI` to our `efi_main` function, which broke the startup translation layer! We also realized GCC was missing `-maccumulate-outgoing-args`, which causes catastrophic stack corruption when making Microsoft ABI calls from Linux-compiled code! We fixed the entry point and added the flag to the `Makefile`.

## 6. The Advanced OS Linter Upgrade
**The Issue:** We didn't want any of those fatal architectural bugs to ever happen again.
**The Fix:** We massively upgraded `linter.ps1` to scan the `Makefile` and source code for:
* Missing `-maccumulate-outgoing-args` or `-j .rodata`.
* Improper usage of standard libc headers like `<stdio.h>`.
* Improper usage of `\n` without `\r` in UEFI strings.

## Conclusion
BlankOS went from a crashing stub to a highly robust, universally compatible, instantly-booting UEFI operating system with a fully automated CI/CD compilation and release pipeline!
