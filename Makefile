CC = gcc
CXX = g++
LD = ld
OBJCOPY = objcopy

CFLAGS = -ffreestanding -fno-stack-protector -fpic -fshort-wchar -mno-red-zone -maccumulate-outgoing-args -DGNU_EFI_USE_MS_ABI -I /usr/include/efi -I /usr/include/efi/x86_64
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti -fpermissive
LDFLAGS = -nostdlib -znocombreloc -shared -Bsymbolic -L /usr/lib -T /usr/lib/elf_x86_64_efi.lds
LIBS = -lefi -lgnuefi

# Kernel Objects MUST be defined before they are used as dependencies!
KERNEL_OBJS = src/kernel/kernel.o src/kernel/memory.o src/kernel/bdrm.o src/kernel/bloe_loader.o src/kernel/blank_reg.o src/kernel/crypto.o src/kernel/time.o src/kernel/notifications.o src/kernel/power.o src/kernel/battery.o src/kernel/cookies.o src/kernel/audio.o src/kernel/stubs.o src/ui/blankUI.o src/apps/setup.o src/apps/login.o src/apps/settings.o src/apps/blankreg_edit.o src/apps/blankpad.o src/apps/updater.o src/apps/blankbrowser.o src/apps/loading_screen.o src/apps/updating_screen.o src/apps/intro.o src/apps/sysinfo.o src/apps/store.o src/apps/blankdrop.o

all: blankOS.iso

# Bootloader Target (Strictly C for GNU-EFI)
src/boot/boot.o: src/boot/boot.c
	$(CC) $(CFLAGS) -m64 -c $< -o $@

src/boot/boot.so: src/boot/boot.o $(KERNEL_OBJS)
	$(LD) $(LDFLAGS) /usr/lib/crt0-efi-x86_64.o src/boot/boot.o $(KERNEL_OBJS) -o $@ $(LIBS)

src/boot/BOOTX64.EFI: src/boot/boot.so
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rel -j .rela -j .rel.* -j .rela.* -j .reloc -j .rodata -j .bss --target efi-app-x86_64 $< $@

# Object Compilation Rules
src/kernel/%.o: src/kernel/%.cpp
	$(CXX) $(CXXFLAGS) -m64 -c $< -o $@

src/ui/%.o: src/ui/%.cpp
	$(CXX) $(CXXFLAGS) -m64 -c $< -o $@

src/apps/%.o: src/apps/%.cpp
	$(CXX) $(CXXFLAGS) -m64 -c $< -o $@

# ISO Generation
.PHONY: all iso clean run

iso: blankOS.iso

src/boot/mbr_stub.bin: src/boot/mbr_stub.asm
	nasm -f bin $< -o $@

blankOS.iso: src/boot/BOOTX64.EFI src/boot/mbr_stub.bin
	rm -rf iso
	mkdir -p iso/EFI/BOOT
	cp src/boot/BOOTX64.EFI iso/EFI/BOOT/BOOTX64.EFI
	cp src/boot/BOOTX64.EFI iso/EFI/BOOT/BOOTIA32.EFI
	cp version.json iso/version.json
	cp src/boot/mbr_stub.bin iso/mbr_stub.bin
	dd if=/dev/zero of=iso/efiboot.img bs=1M count=64
	mformat -i iso/efiboot.img ::
	mmd -i iso/efiboot.img ::/EFI
	mmd -i iso/efiboot.img ::/EFI/BOOT
	mcopy -i iso/efiboot.img iso/EFI/BOOT/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i iso/efiboot.img iso/EFI/BOOT/BOOTIA32.EFI ::/EFI/BOOT/BOOTIA32.EFI
	xorriso -as mkisofs -R -f -e efiboot.img -no-emul-boot -o $@ iso

run: blankOS.iso
	qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -cdrom blankOS.iso -m 2048

clean:
	rm -rf src/boot/*.o src/boot/*.so src/boot/*.EFI src/boot/*.bin src/kernel/*.o src/kernel/*.elf iso blankOS.iso
