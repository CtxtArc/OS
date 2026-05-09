# KDXOS

> A 32-bit graphical, multitasking operating system built entirely from scratch.

KDXOS is a custom-built, hobbyist operating system featuring a dynamic kernel heap, a FAT-compatible file system, a tiling window manager, and its own Turing-complete programming language and on-device compiler. It completely bypasses standard C executables in favor of native, script-to-binary compilation directly within the OS itself.

## 🌟 Core Features

### 🧠 Kernel & Memory Management

* **Custom Bootloader/Multiboot:** Boots via GRUB into a 32-bit protected mode environment.
* **Physical Memory Manager (PMM) & Paging:** Full virtual memory mapping and isolation.
* **Dynamic Kernel Heap (`kheap`):** Custom `kmalloc` and `kfree` implementation with automatic block coalescing, internal fragmentation management, and a massive 64MB memory pool.
* **Preemptive Multitasking:** Context switching via hardware timer interrupts, task scheduling, and isolated 16KB stacks per process.

### 🖥️ Graphics & GUI

* **VESA Graphics Interface:** Double-buffered high-resolution rendering (e.g., 1024x768).
* **Tiling Window Manager:** A dedicated compositor task that handles dynamic screen tiling, window redrawing, and active-focus borders based on spawned GUI tasks.
* **Smart Dirty Rectangles:** Only redraws updated portions of the screen to maintain high performance.

### 💾 File System (FAT Hybrid)

* **IDE Hard Drive Driver:** Custom PIO-mode ATA disk reader/writer.
* **Native File Management:** Full support for creating, reading, writing, appending, and deleting files.
* **Directory Management:** Supports subdirectories, relative paths (`.` and `..`), and absolute path parsing.
* **Dynamic Cluster Allocation:** Automatically grows files across the disk by allocating free clusters and linking them in the File Allocation Table.

### 🐚 The KDXOS Shell

An interactive, graphical command-line interface with a rich set of built-in commands:

* **File I/O:** `LS`, `CD`, `CAT`, `MKDIR`, `RM`, `RMDIR`, `PWD`, `TOUCH`, `WRITE`, `HEXDUMP`
* **Process Management:** `PS`, `KILL`, `TOP`, `RUN`
* **System Diagnostics:** `STAT` (live heap analysis), `UPTIME`, `CLEAR`, `ECHO`
* **Native Apps:** `KED` (Text Editor), `GAME`, `TIMER`

### ⚙️ Native Programming Language

KDXOS features a built-in assembler/compiler. Users can write text files using the custom KDXOS Scripting Language and compile them directly into native x86 machine code binaries on the device.

* **Turing-Complete:** Supports `GOTO`, `CALL`, `RET`, and conditional jumps (`CMP`, `JE`, `JL`, `JG`, `JNE`).
* **Variables & Math:** Dynamic runtime variables (`SET`), `ADD`, `SUB`.
* **Interactive Input:** Non-blocking hardware keyboard hooks (`GETKEY`).
* **Graphics API:** Direct calls to OS compositor (`WINDOW`, `RECT`, `PRINT`).

## 📝 Code Example (KDXOS Language)

Here is a native KDXOS application that renders a menu and waits for keyboard input:

```as
WINDOW 0 0 400 300
CALL draw_menu
SET key 0

LABEL input_loop
    GETKEY key
    CMP key 0
    JE loop_end
    
    CMP key 113  # 'q' to quit
    JE handle_quit
    
LABEL loop_end
    SLEEP 50
    GOTO input_loop

LABEL handle_quit
    RECT 20 150 360 40 0x222222
    PRINT "Exiting application..." 30 160 0xFF0000
    SLEEP 1000
    EXIT

LABEL draw_menu
    RECT 0 0 400 300 0x222222
    PRINT "KDXOS INTERACTIVE MENU" 100 20 0xFFFF00
    PRINT "[Q] Quit Application" 40 130 0xAAAAAA
    RET

```

## 🛠️ Building & Running

### Prerequisites

* `gcc` (i686-elf cross-compiler recommended)
* `nasm`
* `qemu-system-i386` (for emulation)
* `make`

### Compilation

To compile the kernel and build the OS image:

```bash
make clean
make

```

### Running in Emulator

To launch KDXOS inside QEMU with an attached IDE hard drive:

```bash
make run

```

## 📂 Project Structure
```
OS_Root/
├── assets/             # Media, fonts, or external assets (e.g., BG.BMP)
├── bin/                # Compiled userland binaries (e.g., SPIN2.BIN)
├── build/              # Object files (.o) generated during compilation
├── disk.img            # The compiled hard drive image (FAT filesystem)
├── file/               # Raw files to be injected into the disk image
├── include/            # C header files (.h) for the kernel and drivers
├── tests/              # Test scripts or testing framework files
├── linker.ld           # Linker script mapping out kernel memory sections
├── Makefile            # Build system instructions
├── sys_specs.csv       # Language and Syscall specifications documentation
└── src/                # Kernel and Driver Source Code
    ├── assembler.c     # Native KDXOS Scripting Language compiler
    ├── bmp.c           # BMP image parsing and rendering
    ├── boot.s          # Assembly bootloader / Multiboot entry point
    ├── fat.c           # FAT16/32 File System and ATA PIO driver
    ├── font.c          # Bitmap font rendering logic
    ├── gdt.c           # Global Descriptor Table setup in C
    ├── gdt_flush.s     # GDT assembly loader
    ├── idt.c           # Interrupt Descriptor Table & Exception handlers
    ├── interrupts.s    # Assembly wrappers for ISRs and IRQs
    ├── io.c            # Low-level port I/O (inb, outb)
    ├── KED.c           # KDXOS Native Text Editor application
    ├── kernel.c        # Main kernel entry point and initialization
    ├── kheap.c         # Dynamic Memory Manager (kmalloc / kfree)
    ├── lib.c           # Standard library utilities (kmemset, kstrcmp, etc.)
    ├── paging.c        # Virtual memory management
    ├── paging_asm.s    # Assembly routines for enabling paging
    ├── pmm.c           # Physical Memory Manager (bitmap based)
    ├── shell.c         # Graphical command-line interface & commands
    ├── task.c          # Preemptive multitasking scheduler
    └── vesa.c          # VESA Graphics driver and double-buffering
```
## 🚀 Future Roadmap

* [ ] Virtual File System (VFS) abstraction layer.
* [ ] Extended standard library for the custom scripting language.

---

*Built with C, Assembly, and a lot of kernel panics.*
