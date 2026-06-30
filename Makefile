CC = gcc
AS = nasm
# -Iinclude allows using #include "header.h" instead of paths
#CFLAGS = -m32 -ffreestanding -O2 -Wall -Wextra -std=gnu99 -fno-stack-protector -Iinclude
CFLAGS = -m32 \
         -ffreestanding \
         -O0 \
         -Wall -Wextra \
         -std=gnu99 \
         -Iinclude \
         -fno-stack-protector \
         -fno-pic \
         -fno-pie \
         -fno-builtin
LDFLAGS = -T linker.ld -m32 -nostdlib -ffreestanding -Wl,--build-id=none -Wl,-z,noexecstack -no-pie

# Directories
SRCDIR = src
INCDIR = include
BUILDDIR = build
BINDIR = bin

# Find all C and Assembly files in src/
C_SOURCES = $(wildcard $(SRCDIR)/*.c)
S_SOURCES = $(wildcard $(SRCDIR)/*.s)

# Convert source file paths to object file paths in build/
OBJ = $(C_SOURCES:$(SRCDIR)/%.c=$(BUILDDIR)/%.o) \
      $(S_SOURCES:$(SRCDIR)/%.s=$(BUILDDIR)/%.o)

# Final output targets
KERNEL_BIN = $(BINDIR)/myos.bin
KERNEL_ISO = $(BINDIR)/myos.iso

all: prepare $(KERNEL_ISO)

disk.img:
	@echo "Creating 10MB FAT16 Disk..."
	dd if=/dev/zero of=disk.img bs=1M count=10
	sudo mkfs.fat -F 16 disk.img

disk: disk.img
	@echo "Injecting files into disk.img..."
	mkdir -p tests assets
	python3 -c "print('A' * 512 + 'B' * 512)" > tests/large.txt
	
	# Create the /tests directory on the image (lowercase)
	-mmd -i disk.img ::/tests
	
	# Inject tests/ folder files preserving original case
	for file in tests/*.txt; do \
		mcopy -o -i disk.img $$file ::/tests/; \
	done
	
	# Inject bg.bmp preserving case
	@if [ -f assets/bg.bmp ]; then \
		echo "Injecting assets/bg.bmp into disk root..."; \
		mcopy -o -i disk.img assets/bg.bmp ::/bg.bmp; \
	elif [ -f assets/BG.BMP ]; then \
		echo "Injecting assets/BG.BMP into disk root..."; \
		mcopy -o -i disk.img assets/BG.BMP ::/BG.BMP; \
	else \
		@echo "WARNING: No background image found in assets/! OS will fallback to solid color."; \
	fi


lsdisk:
	@echo "FAT16 Root Directory Listing:"
	mdir -i disk.img ::/
lstest:
	@echo "FAT16 TESTS/ Directory Listing:"
	mdir -i disk.img ::/TESTS

clean_disk:
	rm -f disk.img
	


# Create the directories if they don't exist
prepare:
	@mkdir -p $(BUILDDIR)
	@mkdir -p $(BINDIR)

# Rule for C files
$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Rule for Assembly files
$(BUILDDIR)/%.o: $(SRCDIR)/%.s
	$(AS) -f elf32 $< -o $@

# Linking the final binary into bin/
$(KERNEL_BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $(KERNEL_BIN) $(OBJ)

# Building the ISO into bin/
$(KERNEL_ISO): $(KERNEL_BIN)
	@mkdir -p isodir/boot/grub
	cp $(KERNEL_BIN) isodir/boot/myos.bin
	@echo 'set timeout=0' > isodir/boot/grub/grub.cfg
	@echo 'set default=0' >> isodir/boot/grub/grub.cfg
	@echo 'menuentry "myos" { multiboot /boot/myos.bin }' >> isodir/boot/grub/grub.cfg
	grub-mkrescue -o $(KERNEL_ISO) isodir
	@rm -rf isodir

run:
	qemu-system-i386 -cdrom $(KERNEL_ISO) -hda disk.img -boot d \
	-vga std \
	-display sdl \
	-m 256

clean:
	rm -rf $(BUILDDIR) $(BINDIR) isodir
