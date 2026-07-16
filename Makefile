CC = gcc
LD = ld
OBJCOPY = objcopy

CFLAGS = -ffreestanding -fno-stack-protector -fpic -fshort-wchar -mno-red-zone -I /usr/include/efi -I /usr/include/efi/x86_64
LDFLAGS = -nostdlib -shared -Bsymbolic -L /usr/lib -T /usr/lib/elf_x86_64_efi.lds
LIBS = -lefi -lgnuefi

all: blankOS.iso

# Bootloader
src/boot/boot.so: src/boot/boot.c
	$(CC) $(CFLAGS) -c $< -o src/boot/boot.o
	$(LD) $(LDFLAGS) /usr/lib/crt0-efi-x86_64.o src/boot/boot.o -o $@ $(LIBS)

src/boot/BOOTX64.EFI: src/boot/boot.so
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rel -j .rela -j .rel.* -j .rela.* -j .reloc --target efi-app-x86_64 $< $@

# Kernel
KERNEL_OBJS = src/kernel/kernel_entry.o src/kernel/kernel.o src/kernel/memory.o src/kernel/bdrm.o src/kernel/bloe_loader.o src/kernel/blank_reg.o src/kernel/crypto.o src/kernel/time.o src/kernel/notifications.o src/apps/updater.o src/apps/blankbrowser.o

src/kernel/kernel_entry.o: src/kernel/kernel_entry.asm
	nasm -f elf64 $< -o $@

src/kernel/%.o: src/kernel/%.c
	$(CC) -ffreestanding -mno-red-zone -m64 -c $< -o $@

src/apps/%.o: src/apps/%.c
	$(CC) -ffreestanding -mno-red-zone -m64 -c $< -o $@

src/kernel/kernel.elf: $(KERNEL_OBJS)
	$(LD) -nostdlib -Ttext 0x100000 $^ -o $@

# ISO Generation
.PHONY: all iso clean run

iso: blankOS.iso

blankOS.iso: src/boot/BOOTX64.EFI src/kernel/kernel.elf
	mkdir -p iso/EFI/BOOT
	cp src/boot/BOOTX64.EFI iso/EFI/BOOT/BOOTX64.EFI
	cp src/kernel/kernel.elf iso/kernel.elf
	cp version.json iso/version.json
	xorriso -as mkisofs -R -f -e EFI/BOOT/BOOTX64.EFI -no-emul-boot -o $@ iso

run: blankOS.iso
	qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -cdrom blankOS.iso -m 2048

clean:
	rm -rf src/boot/*.o src/boot/*.so src/boot/*.EFI src/kernel/*.o src/kernel/*.elf iso blankOS.iso
