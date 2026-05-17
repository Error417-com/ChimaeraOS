# ChimaeraOS Build System
# Targets: fsck-test-iso, integration-test-iso, lfn-test-iso, clean
#
# Two-pass build
# --------------
# Each ISO target performs two link passes:
#   Pass 1: link all .o files (without symbol blobs) → build/kernel_pass1.elf
#   Embed:  tools/embed_symtab.sh extracts .symtab/.strtab from pass-1 ELF
#           and produces build/symtab_blob.o and build/strtab_blob.o
#   Pass 2: link all .o files + symbol blobs → iso/boot/kernel.elf
#
# This ensures the panic handler has a complete, in-memory symbol table.
#
# fat32.c has been split into three files:
#   fat32_core.c  — volume state, sector I/O, FAT/dir helpers, mount
#   fat32_ops.c   — public file and directory operations
#   fat32_fsck.c  — FSCK bridge API
# All three share fat32_internal.h (internal types, globals, helpers).

CC      = gcc
AS      = nasm
LD      = ld
GRUB    = grub-mkrescue

# -fno-omit-frame-pointer is required for the panic stack walker.
CFLAGS  = -m32 -ffreestanding -fno-stack-protector -fno-builtin \
          -fno-pie -fno-pic -O2 -Wall -Wextra \
          -fno-omit-frame-pointer \
          -I src/include

ASFLAGS = -f elf32

LDFLAGS = -m elf_i386 -T src/kernel/linker.ld --oformat elf32-i386

OBJDIR  = build

# ── Common objects (shared by all targets) ────────────────────────────────────

$(OBJDIR)/boot.o: src/kernel/boot.asm | $(OBJDIR)
	$(AS) $(ASFLAGS) $< -o $@

$(OBJDIR)/serial.o: src/drivers/serial.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/vga.o: src/drivers/vga.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/ata.o: src/drivers/ata.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/mm.o: src/mm/mm.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/panic.o: src/kernel/panic.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/timer.o: src/kernel/timer.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/metrics.o: src/kernel/metrics.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)

# ── Integration test ISO ──────────────────────────────────────────────────────

$(OBJDIR)/fat32_core_int.o: src/fs/fat32_core.c src/fs/fat32_internal.h | $(OBJDIR)
	$(CC) $(CFLAGS) -DINTEGRATION_TEST -c $< -o $@

$(OBJDIR)/fat32_ops_int.o: src/fs/fat32_ops.c src/fs/fat32_internal.h | $(OBJDIR)
	$(CC) $(CFLAGS) -DINTEGRATION_TEST -c $< -o $@

$(OBJDIR)/fat32_fsck_int.o: src/fs/fat32_fsck.c src/fs/fat32_internal.h | $(OBJDIR)
	$(CC) $(CFLAGS) -DINTEGRATION_TEST -c $< -o $@

$(OBJDIR)/fsck_int.o: src/fs/fsck.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DINTEGRATION_TEST -c $< -o $@

$(OBJDIR)/kernel_int.o: src/kernel/kernel_integration.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DINTEGRATION_TEST -c $< -o $@

INT_OBJS_CORE = $(OBJDIR)/boot.o \
                $(OBJDIR)/serial.o \
                $(OBJDIR)/vga.o \
                $(OBJDIR)/ata.o \
                $(OBJDIR)/mm.o \
                $(OBJDIR)/fat32_core_int.o \
                $(OBJDIR)/fat32_ops_int.o \
                $(OBJDIR)/fat32_fsck_int.o \
                $(OBJDIR)/fsck_int.o \
                $(OBJDIR)/panic.o \
                $(OBJDIR)/timer.o \
                $(OBJDIR)/metrics.o \
                $(OBJDIR)/kernel_int.o

integration-test-iso: $(INT_OBJS_CORE)
	@mkdir -p iso/boot
	# Pass 1: link without symbol blobs to get the symbol table
	$(LD) $(LDFLAGS) -o $(OBJDIR)/kernel_int_pass1.elf $(INT_OBJS_CORE)
	# Embed: extract .symtab/.strtab and produce blob .o files
	bash tools/embed_symtab.sh $(OBJDIR)/kernel_int_pass1.elf \
	    $(OBJDIR)/symtab_int_blob.o $(OBJDIR)/strtab_int_blob.o
	# Pass 2: link with symbol blobs embedded
	$(LD) $(LDFLAGS) -o iso/boot/kernel.elf \
	    $(INT_OBJS_CORE) \
	    $(OBJDIR)/symtab_int_blob.o $(OBJDIR)/strtab_int_blob.o
	$(GRUB) -o chimaera_integration_test.iso iso/
	@echo "[BUILD] chimaera_integration_test.iso ready"

# ── FSCK test ISO ─────────────────────────────────────────────────────────────

$(OBJDIR)/fat32_core_fsck.o: src/fs/fat32_core.c src/fs/fat32_internal.h | $(OBJDIR)
	$(CC) $(CFLAGS) -DFSCK_TEST -c $< -o $@

$(OBJDIR)/fat32_ops_fsck.o: src/fs/fat32_ops.c src/fs/fat32_internal.h | $(OBJDIR)
	$(CC) $(CFLAGS) -DFSCK_TEST -c $< -o $@

$(OBJDIR)/fat32_fsck_fsck.o: src/fs/fat32_fsck.c src/fs/fat32_internal.h | $(OBJDIR)
	$(CC) $(CFLAGS) -DFSCK_TEST -c $< -o $@

$(OBJDIR)/fsck_fsck.o: src/fs/fsck.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DFSCK_TEST -c $< -o $@

$(OBJDIR)/kernel_fsck.o: src/kernel/kernel.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DFSCK_TEST -c $< -o $@

FSCK_OBJS_CORE = $(OBJDIR)/boot.o \
                 $(OBJDIR)/serial.o \
                 $(OBJDIR)/vga.o \
                 $(OBJDIR)/ata.o \
                 $(OBJDIR)/mm.o \
                 $(OBJDIR)/fat32_core_fsck.o \
                 $(OBJDIR)/fat32_ops_fsck.o \
                 $(OBJDIR)/fat32_fsck_fsck.o \
                 $(OBJDIR)/fsck_fsck.o \
                 $(OBJDIR)/panic.o \
                 $(OBJDIR)/timer.o \
                 $(OBJDIR)/metrics.o \
                 $(OBJDIR)/kernel_fsck.o

fsck-test-iso: $(FSCK_OBJS_CORE)
	@mkdir -p iso/boot
	$(LD) $(LDFLAGS) -o $(OBJDIR)/kernel_fsck_pass1.elf $(FSCK_OBJS_CORE)
	bash tools/embed_symtab.sh $(OBJDIR)/kernel_fsck_pass1.elf \
	    $(OBJDIR)/symtab_fsck_blob.o $(OBJDIR)/strtab_fsck_blob.o
	$(LD) $(LDFLAGS) -o iso/boot/kernel.elf \
	    $(FSCK_OBJS_CORE) \
	    $(OBJDIR)/symtab_fsck_blob.o $(OBJDIR)/strtab_fsck_blob.o
	$(GRUB) -o chimaera_fsck_test.iso iso/
	@echo "[BUILD] chimaera_fsck_test.iso ready"

# ── LFN test ISO ──────────────────────────────────────────────────────────────

$(OBJDIR)/fat32_core_lfn.o: src/fs/fat32_core.c src/fs/fat32_internal.h | $(OBJDIR)
	$(CC) $(CFLAGS) -DLFN_TEST -c $< -o $@

$(OBJDIR)/fat32_ops_lfn.o: src/fs/fat32_ops.c src/fs/fat32_internal.h | $(OBJDIR)
	$(CC) $(CFLAGS) -DLFN_TEST -c $< -o $@

$(OBJDIR)/fat32_fsck_lfn.o: src/fs/fat32_fsck.c src/fs/fat32_internal.h | $(OBJDIR)
	$(CC) $(CFLAGS) -DLFN_TEST -c $< -o $@

$(OBJDIR)/fsck_lfn.o: src/fs/fsck.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DLFN_TEST -c $< -o $@

$(OBJDIR)/kernel_lfn.o: src/kernel/kernel_lfn.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DLFN_TEST -c $< -o $@

LFN_OBJS_CORE = $(OBJDIR)/boot.o \
                $(OBJDIR)/serial.o \
                $(OBJDIR)/vga.o \
                $(OBJDIR)/ata.o \
                $(OBJDIR)/mm.o \
                $(OBJDIR)/fat32_core_lfn.o \
                $(OBJDIR)/fat32_ops_lfn.o \
                $(OBJDIR)/fat32_fsck_lfn.o \
                $(OBJDIR)/fsck_lfn.o \
                $(OBJDIR)/panic.o \
                $(OBJDIR)/timer.o \
                $(OBJDIR)/metrics.o \
                $(OBJDIR)/kernel_lfn.o

lfn-test-iso: $(LFN_OBJS_CORE)
	@mkdir -p iso/boot
	$(LD) $(LDFLAGS) -o $(OBJDIR)/kernel_lfn_pass1.elf $(LFN_OBJS_CORE)
	bash tools/embed_symtab.sh $(OBJDIR)/kernel_lfn_pass1.elf \
	    $(OBJDIR)/symtab_lfn_blob.o $(OBJDIR)/strtab_lfn_blob.o
	$(LD) $(LDFLAGS) -o iso/boot/kernel.elf \
	    $(LFN_OBJS_CORE) \
	    $(OBJDIR)/symtab_lfn_blob.o $(OBJDIR)/strtab_lfn_blob.o
	$(GRUB) -o chimaera_lfn_test.iso iso/
	@echo "[BUILD] chimaera_lfn_test.iso ready"

# ── Scheduler demo ISO ───────────────────────────────────────────────────────

$(OBJDIR)/idt.o: src/kernel/idt.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DSCHED_DEMO -c $< -o $@

$(OBJDIR)/idt_asm.o: src/kernel/idt.asm | $(OBJDIR)
	$(AS) $(ASFLAGS) $< -o $@

$(OBJDIR)/context_switch.o: src/proc/context_switch.asm | $(OBJDIR)
	$(AS) $(ASFLAGS) $< -o $@

$(OBJDIR)/sched.o: src/proc/sched.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DSCHED_DEMO -c $< -o $@

$(OBJDIR)/fat32_core_sched.o: src/fs/fat32_core.c src/fs/fat32_internal.h | $(OBJDIR)
	$(CC) $(CFLAGS) -DSCHED_DEMO -c $< -o $@

$(OBJDIR)/fat32_ops_sched.o: src/fs/fat32_ops.c src/fs/fat32_internal.h | $(OBJDIR)
	$(CC) $(CFLAGS) -DSCHED_DEMO -c $< -o $@

$(OBJDIR)/fat32_fsck_sched.o: src/fs/fat32_fsck.c src/fs/fat32_internal.h | $(OBJDIR)
	$(CC) $(CFLAGS) -DSCHED_DEMO -c $< -o $@

$(OBJDIR)/fsck_sched.o: src/fs/fsck.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DSCHED_DEMO -c $< -o $@

$(OBJDIR)/kernel_sched.o: src/kernel/kernel_sched.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DSCHED_DEMO -c $< -o $@

SCHED_OBJS_CORE = $(OBJDIR)/boot.o \
                  $(OBJDIR)/serial.o \
                  $(OBJDIR)/vga.o \
                  $(OBJDIR)/ata.o \
                  $(OBJDIR)/mm.o \
                  $(OBJDIR)/fat32_core_sched.o \
                  $(OBJDIR)/fat32_ops_sched.o \
                  $(OBJDIR)/fat32_fsck_sched.o \
                  $(OBJDIR)/fsck_sched.o \
                  $(OBJDIR)/panic.o \
                  $(OBJDIR)/timer.o \
                  $(OBJDIR)/idt.o \
                  $(OBJDIR)/idt_asm.o \
                  $(OBJDIR)/context_switch.o \
                  $(OBJDIR)/sched.o \
                  $(OBJDIR)/kernel_sched.o

sched-demo-iso: $(SCHED_OBJS_CORE)
	@mkdir -p iso/boot
	$(LD) $(LDFLAGS) -o $(OBJDIR)/kernel_sched_pass1.elf $(SCHED_OBJS_CORE)
	bash tools/embed_symtab.sh $(OBJDIR)/kernel_sched_pass1.elf \
	    $(OBJDIR)/symtab_sched_blob.o $(OBJDIR)/strtab_sched_blob.o
	$(LD) $(LDFLAGS) -o iso/boot/kernel.elf \
	    $(SCHED_OBJS_CORE) \
	    $(OBJDIR)/symtab_sched_blob.o $(OBJDIR)/strtab_sched_blob.o
	$(GRUB) -o chimaera_sched_demo.iso iso/
	@echo "[BUILD] chimaera_sched_demo.iso ready"

# ── USB demo ISO ─────────────────────────────────────────────────────────────

$(OBJDIR)/pci.o: src/drivers/usb/pci.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DUSB_DEMO -c $< -o $@

$(OBJDIR)/uhci.o: src/drivers/usb/uhci.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DUSB_DEMO -c $< -o $@

$(OBJDIR)/usb_core.o: src/drivers/usb/usb_core.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DUSB_DEMO -c $< -o $@

$(OBJDIR)/hid.o: src/drivers/usb/hid.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DUSB_DEMO -c $< -o $@

$(OBJDIR)/hid_kbd.o: src/drivers/usb/hid_kbd.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DUSB_DEMO -c $< -o $@

$(OBJDIR)/hid_mouse.o: src/drivers/usb/hid_mouse.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DUSB_DEMO -c $< -o $@

$(OBJDIR)/fat32_core_usb.o: src/fs/fat32_core.c src/fs/fat32_internal.h | $(OBJDIR)
	$(CC) $(CFLAGS) -DUSB_DEMO -c $< -o $@

$(OBJDIR)/fat32_ops_usb.o: src/fs/fat32_ops.c src/fs/fat32_internal.h | $(OBJDIR)
	$(CC) $(CFLAGS) -DUSB_DEMO -c $< -o $@

$(OBJDIR)/fat32_fsck_usb.o: src/fs/fat32_fsck.c src/fs/fat32_internal.h | $(OBJDIR)
	$(CC) $(CFLAGS) -DUSB_DEMO -c $< -o $@

$(OBJDIR)/fsck_usb.o: src/fs/fsck.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DUSB_DEMO -c $< -o $@

$(OBJDIR)/idt_usb.o: src/kernel/idt.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DUSB_DEMO -c $< -o $@

$(OBJDIR)/idt_asm_usb.o: src/kernel/idt.asm | $(OBJDIR)
	$(AS) $(ASFLAGS) $< -o $@

$(OBJDIR)/context_switch_usb.o: src/proc/context_switch.asm | $(OBJDIR)
	$(AS) $(ASFLAGS) $< -o $@

$(OBJDIR)/sched_usb.o: src/proc/sched.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DUSB_DEMO -c $< -o $@

$(OBJDIR)/kernel_usb.o: src/kernel/kernel_usb.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DUSB_DEMO -c $< -o $@

USB_OBJS_CORE = $(OBJDIR)/boot.o \
                $(OBJDIR)/serial.o \
                $(OBJDIR)/vga.o \
                $(OBJDIR)/ata.o \
                $(OBJDIR)/mm.o \
                $(OBJDIR)/fat32_core_usb.o \
                $(OBJDIR)/fat32_ops_usb.o \
                $(OBJDIR)/fat32_fsck_usb.o \
                $(OBJDIR)/fsck_usb.o \
                $(OBJDIR)/panic.o \
                $(OBJDIR)/timer.o \
                $(OBJDIR)/idt_usb.o \
                $(OBJDIR)/idt_asm_usb.o \
                $(OBJDIR)/context_switch_usb.o \
                $(OBJDIR)/sched_usb.o \
                $(OBJDIR)/pci.o \
                $(OBJDIR)/uhci.o \
                $(OBJDIR)/usb_core.o \
                $(OBJDIR)/hid.o \
                $(OBJDIR)/hid_kbd.o \
                $(OBJDIR)/hid_mouse.o \
                $(OBJDIR)/kernel_usb.o

usb-demo-iso: $(USB_OBJS_CORE)
	@mkdir -p iso/boot
	$(LD) $(LDFLAGS) -o $(OBJDIR)/kernel_usb_pass1.elf $(USB_OBJS_CORE)
	bash tools/embed_symtab.sh $(OBJDIR)/kernel_usb_pass1.elf \
	    $(OBJDIR)/symtab_usb_blob.o $(OBJDIR)/strtab_usb_blob.o
	$(LD) $(LDFLAGS) -o iso/boot/kernel.elf \
	    $(USB_OBJS_CORE) \
	    $(OBJDIR)/symtab_usb_blob.o $(OBJDIR)/strtab_usb_blob.o
	$(GRUB) -o chimaera_usb_demo.iso iso/
	@echo "[BUILD] chimaera_usb_demo.iso ready"

# ── GUI compositor demo ISO ──────────────────────────────────────────────────
$(OBJDIR)/compositor.o: src/gui/compositor.c src/gui/compositor.h | $(OBJDIR)
	$(CC) $(CFLAGS) -DGUI_DEMO -I src/gui -c $< -o $@

$(OBJDIR)/idt_gui.o: src/kernel/idt.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DGUI_DEMO -c $< -o $@

$(OBJDIR)/idt_asm_gui.o: src/kernel/idt.asm | $(OBJDIR)
	$(AS) $(ASFLAGS) $< -o $@

$(OBJDIR)/context_switch_gui.o: src/proc/context_switch.asm | $(OBJDIR)
	$(AS) $(ASFLAGS) $< -o $@

$(OBJDIR)/sched_gui.o: src/proc/sched.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DGUI_DEMO -c $< -o $@

$(OBJDIR)/kernel_gui.o: src/kernel/kernel_gui.c src/gui/compositor.h | $(OBJDIR)
	$(CC) $(CFLAGS) -DGUI_DEMO -I src/gui -c $< -o $@

GUI_OBJS_CORE = $(OBJDIR)/boot.o \
                $(OBJDIR)/serial.o \
                $(OBJDIR)/vga.o \
                $(OBJDIR)/mm.o \
                $(OBJDIR)/panic.o \
                $(OBJDIR)/timer.o \
                $(OBJDIR)/idt_gui.o \
                $(OBJDIR)/idt_asm_gui.o \
                $(OBJDIR)/context_switch_gui.o \
                $(OBJDIR)/sched_gui.o \
                $(OBJDIR)/compositor.o \
                $(OBJDIR)/kernel_gui.o

gui-demo-iso: $(GUI_OBJS_CORE)
	@mkdir -p iso/boot
	$(LD) $(LDFLAGS) -o $(OBJDIR)/kernel_gui_pass1.elf $(GUI_OBJS_CORE)
	bash tools/embed_symtab.sh $(OBJDIR)/kernel_gui_pass1.elf \
	    $(OBJDIR)/symtab_gui_blob.o $(OBJDIR)/strtab_gui_blob.o
	$(LD) $(LDFLAGS) -o iso/boot/kernel.elf \
	    $(GUI_OBJS_CORE) \
	    $(OBJDIR)/symtab_gui_blob.o $(OBJDIR)/strtab_gui_blob.o
	$(GRUB) -o chimaera_gui_demo.iso iso/
	@echo "[BUILD] chimaera_gui_demo.iso ready"

# ── ACPI demo ISO ────────────────────────────────────────────────────────────────
$(OBJDIR)/acpi.o: src/kernel/acpi.c src/include/acpi.h | $(OBJDIR)
	$(CC) $(CFLAGS) -DACPI_DEMO -c $< -o $@
$(OBJDIR)/kernel_acpi.o: src/kernel/kernel_acpi.c src/include/acpi.h | $(OBJDIR)
	$(CC) $(CFLAGS) -DACPI_DEMO -c $< -o $@
$(OBJDIR)/mm_acpi.o: src/mm/mm.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DACPI_DEMO -c $< -o $@
$(OBJDIR)/panic_acpi.o: src/kernel/panic.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DACPI_DEMO -c $< -o $@
$(OBJDIR)/timer_acpi.o: src/kernel/timer.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DACPI_DEMO -c $< -o $@

ACPI_OBJS_CORE = $(OBJDIR)/boot.o \
                 $(OBJDIR)/serial.o \
                 $(OBJDIR)/vga.o \
                 $(OBJDIR)/mm_acpi.o \
                 $(OBJDIR)/panic_acpi.o \
                 $(OBJDIR)/timer_acpi.o \
                 $(OBJDIR)/acpi.o \
                 $(OBJDIR)/kernel_acpi.o

acpi-demo-iso: $(ACPI_OBJS_CORE)
	@mkdir -p iso/boot
	$(LD) $(LDFLAGS) -o $(OBJDIR)/kernel_acpi_pass1.elf $(ACPI_OBJS_CORE)
	bash tools/embed_symtab.sh $(OBJDIR)/kernel_acpi_pass1.elf \
	    $(OBJDIR)/symtab_acpi_blob.o $(OBJDIR)/strtab_acpi_blob.o
	$(LD) $(LDFLAGS) -o iso/boot/kernel.elf \
	    $(ACPI_OBJS_CORE) \
	    $(OBJDIR)/symtab_acpi_blob.o $(OBJDIR)/strtab_acpi_blob.o
	$(GRUB) -o chimaera_acpi_demo.iso iso/
	@echo "[BUILD] chimaera_acpi_demo.iso ready"

# ── Clean ──────────────────────────────────────────────────────────────────────
clean:
	rm -rf $(OBJDIR) iso/boot/kernel.elf \
	    chimaera_fsck_test.iso chimaera_lfn_test.iso chimaera_integration_test.iso \
	    chimaera_sched_demo.iso chimaera_usb_demo.iso chimaera_gui_demo.iso \
	    chimaera_acpi_demo.iso
.PHONY: fsck-test-iso lfn-test-iso integration-test-iso sched-demo-iso usb-demo-iso gui-demo-iso acpi-demo-iso clean
