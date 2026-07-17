CC = gcc
CXX = g++
LD = ld
OBJCOPY = objcopy

CFLAGS = -ffreestanding -fno-stack-protector -fpic -fshort-wchar -mno-red-zone -maccumulate-outgoing-args -DGNU_EFI_USE_MS_ABI -I /usr/include/efi -I /usr/include/efi/x86_64
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti -fpermissive
LDFLAGS = -nostdlib -znocombreloc -shared -Bsymbolic -L /usr/lib -T /usr/lib/elf_x86_64_efi.lds
LIBS = -lefi -lgnuefi

# Kernel Objects MUST be defined before they are used as dependencies!
KERNEL_OBJS = src/kernel/kernel.o src/kernel/stb_stdlib.o src/kernel/stb_wrapper.o src/kernel/memory.o src/kernel/bdrm.o src/kernel/bloe_loader.o src/kernel/elf_loader.o src/kernel/pci.o src/kernel/blank_reg.o src/kernel/crypto.o src/kernel/time.o src/kernel/notifications.o src/kernel/power.o src/kernel/battery.o src/kernel/cookies.o src/kernel/audio.o src/kernel/disk_formatter.o src/kernel/svga.o src/kernel/stubs.o src/ui/compositor.o src/ui/blankDUI.o src/ui/blankUI.o src/apps/setup.o src/apps/login.o src/apps/blankreg_edit.o src/apps/blankpad.o src/apps/updater.o src/apps/loading_screen.o src/apps/updating_screen.o src/apps/intro.o src/apps/sysinfo.o src/apps/store.o src/apps/task_manager.o src/apps/terminal.o src/apps/calculator.o src/apps/weather.o src/apps/browser.o

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
	rm -rf store_repo
	git clone https://github.com/airskye1/blankOS-App-Store store_repo
	cd store_repo && $(MAKE) CXXFLAGS="-ffreestanding -fno-stack-protector -fpic -fpie -mno-red-zone -m64 -c -fno-exceptions -fno-rtti -I /usr/include/efi -I /usr/include/efi/x86_64"
	rm -rf iso
	mkdir -p iso/EFI/BOOT
	mkdir -p iso/EFI/APPS
	mkdir -p iso/assets
	cp assets/* iso/assets/ 2>/dev/null || true
	cp src/boot/BOOTX64.EFI iso/EFI/BOOT/BOOTX64.EFI
	cp store_repo/apps/*.elf iso/EFI/APPS/ 2>/dev/null || true
	cp src/boot/BOOTX64.EFI iso/EFI/BOOT/BOOTIA32.EFI
	cp version.json iso/version.json
	cp src/boot/mbr_stub.bin iso/mbr_stub.bin
	dd if=/dev/zero of=iso/efiboot.img bs=1M count=64
	mformat -i iso/efiboot.img ::
	mmd -i iso/efiboot.img ::/EFI
	mmd -i iso/efiboot.img ::/EFI/BOOT
	mcopy -i iso/efiboot.img iso/EFI/BOOT/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i iso/efiboot.img iso/EFI/BOOT/BOOTIA32.EFI ::/EFI/BOOT/BOOTIA32.EFI
	mmd -i iso/efiboot.img ::/EFI/APPS
	mcopy -i iso/efiboot.img iso/EFI/APPS/*.elf ::/EFI/APPS/ 2>/dev/null || true
	mcopy -i iso/efiboot.img store_repo/catalog.json ::/EFI/APPS/catalog.json 2>/dev/null || true
	mmd -i iso/efiboot.img ::/assets
	mcopy -i iso/efiboot.img iso/assets/* ::/assets/ 2>/dev/null || true
	xorriso -as mkisofs -R -f -e efiboot.img -no-emul-boot -o $@ iso

run: blankOS.iso
	qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -cdrom blankOS.iso -m 2048

clean:
	rm -rf src/boot/*.o src/boot/*.so src/boot/*.EFI src/boot/*.bin src/kernel/*.o src/kernel/*.elf iso blankOS.iso
