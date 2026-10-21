#!/bin/bash
# Build the shim and wrap it in a boot header v4 image for the DM3Q (S23 Ultra) ABL.
# The DTB stays in the stock vendor_boot. ABL builds the initrd from vendor_boot's
# ramdisk + init_boot's (it ignores boot's), so an xnu payload given as $1 is
# linked into the image itself (shim.ld .payload, found by loader.c).
set -euo pipefail
cd "$(dirname "$0")"
CC=${CROSS:-aarch64-elf-}gcc
OBJCOPY=${CROSS:-aarch64-elf-}objcopy
MKBOOTIMG="python3 ../../tools/mkbootimg/mkbootimg.py"
CFLAGS="${DIAG:+-DDIAG_$(echo $DIAG | tr a-z A-Z)} ${LOGGREP_PAT:+-DLOGGREP_PAT=\"$LOGGREP_PAT\"} -O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
mkdir -p out
for f in start.S shim.c console.c mmu.c fdt.c loader.c ufs.c rpmh.c ufsphy.c touch.c perf.c usb.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
done
# The image runs wherever ABL puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$(${CROSS:-aarch64-elf-}readelf -r out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o out/rpmh.o out/ufsphy.o out/touch.o out/perf.o out/usb.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
# xnu payload (tools/mkpayload.py) if given as $1; without one the shim runs its diagnostics
if [ -n "${1:-}" ]; then
	printf '\t.section .payload, "a"\n\t.incbin "%s"\n' "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")" > out/payload.S
else
	printf '\t.section .payload, "a"\n' > out/payload.S
fi
$CC $CFLAGS -c out/payload.S -o out/payload.o
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o out/rpmh.o out/ufsphy.o out/touch.o out/perf.o out/usb.o out/payload.o \
	-o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$(${CROSS:-aarch64-elf-}nm out/shim.elf | awk '$3=="_end"{print $1}')
truncate -s $((16#$size)) out/Image
# Empty newc cpio (ABL dislikes a zero-size ramdisk on some builds); unused on DM3Q
(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
# Header fields of the phone's own build (S918BXXS8DYDC); load addresses come from vendor_boot
$MKBOOTIMG --header_version 4 --kernel out/Image --ramdisk out/ramdisk.bin \
	--os_version 13.0.0 --os_patch_level 2025-05 -o out/shim-boot.img
# Host render of the console layout.
gcc -O1 -Wall -o out/host_test console.c host_test.c && ./out/host_test
ls -l out/Image out/shim-boot.img
