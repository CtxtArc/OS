CC = gcc
AS = nasm
# -Iinclude allows using #include "header.h" instead of paths
CFLAGS = -m32 -ffreestanding -O2 -Wall -Wextra -std=gnu99 -Iinclude
LDFLAGS = -T linker.ld -m32 -nostdlib -ffreestanding -Wl,--build-id=none -no-pie

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
	# mcopy -o overwrites if it exists
	mcopy -i disk.img test.txt ::/TEST.TXT
	# Create a 2KB dummy file on your host
	python3 -c "print('A' * 512 + 'B' * 512)" > LARGE.TXT
	mcopy -i disk.img LARGE.TXT ::/LARGE.TXT
	mcopy -i disk.img program.txt ::/PROGRAM.TXT
	mcopy -i disk.img print.txt ::/PRINT.TXT
	mcopy -i disk.img label.txt ::/LABEL.TXT
lsdisk:
	@echo "FAT16 Root Directory Listing:"
	mdir -i disk.img ::/

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
	qemu-system-i386 -cdrom $(KERNEL_ISO) -hda disk.img -boot d

clean:
	rm -rf $(BUILDDIR) $(BINDIR) isodir
