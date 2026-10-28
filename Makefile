
CC = gcc

KERNEL_SOURCE = kernel/main.c

CFLAGS = -std=gnu11 -O2 -Wall -Wextra \
         -ffreestanding -fno-stack-protector \
         -fno-pic -fno-pie -m64 -mno-red-zone \
         -mcmodel=kernel -mno-mmx -mno-sse -mno-sse2 \
         -Ilimine

LDFLAGS = -nostdlib -static -no-pie \
          -Wl,-T,linker.ld \
          -Wl,--build-id=none \
          -Wl,-z,max-page-size=0x1000

.PHONY: all iso clean

all: kernel.elf

kernel.elf: $(KERNEL_SOURCE) linker.ld limine/limine.h
	@test -f "$(KERNEL_SOURCE)" || \
	  (echo "ERROR: Kernel source not found: $(KERNEL_SOURCE)"; exit 1)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(KERNEL_SOURCE)

iso: kernel.elf
	mkdir -p iso_root/boot iso_root/EFI/BOOT

	cp kernel.elf iso_root/boot/kernel.elf
	cp limine.conf iso_root/limine.conf

	cp limine/limine-bios.sys iso_root/
	cp limine/limine-bios-cd.bin iso_root/
	cp limine/limine-uefi-cd.bin iso_root/
	cp limine/BOOTX64.EFI iso_root/EFI/BOOT/

	xorriso -as mkisofs -R -r -J \
		-b limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 \
		-boot-info-table \
		--efi-boot limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image \
		--protective-msdos-label \
		iso_root -o veritasos.iso

	./limine/limine bios-install veritasos.iso

clean:
	rm -rf kernel.elf veritasos.iso iso_root
