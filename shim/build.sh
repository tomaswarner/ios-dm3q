#!/bin/bash
# Build the shim and wrap it in a boot header v4 image for the DM3Q (S23 Ultra) ABL.
# The DTB stays in the stock vendor_boot; ABL loads vendor ramdisk + ours back to back.
set -euo pipefail
cd "$(dirname "$0")"
CC=${CROSS:-aarch64-elf-}gcc
OBJCOPY=${CROSS:-aarch64-elf-}objcopy
MKBOOTIMG="python3 ../../tools/mkbootimg/mkbootimg.py"
# Vendor ramdisk size of the phone's build (S918BXXS8DYDC vendor_boot header): our ramdisk
# follows it directly, so pad the payload to land 8-byte aligned (loader.c scans in 8s).
VENDOR_RAMDISK_SIZE=${VENDOR_RAMDISK_SIZE:-14058910}
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
mkdir -p out
for f in start.S shim.c console.c mmu.c fdt.c loader.c ufs.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
done
# The image runs wherever ABL puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$(${CROSS:-aarch64-elf-}readelf -r out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$(${CROSS:-aarch64-elf-}nm out/shim.elf | awk '$3=="_end"{print $1}')
truncate -s $((16#$size)) out/Image
# Ramdisk: an xnu payload (tools/mkpayload.py) if given as $1, else an empty
# newc cpio (ABL dislikes a zero-size ramdisk on some builds) -> diagnostics.
if [ -n "${1:-}" ]; then
	pad=$(( (8 - VENDOR_RAMDISK_SIZE % 8) % 8 ))
	{ head -c $pad /dev/zero; cat "$1"; } > out/ramdisk.bin
else
	(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
fi
# Header fields of the phone's own build (S918BXXS8DYDC); load addresses come from vendor_boot
$MKBOOTIMG --header_version 4 --kernel out/Image --ramdisk out/ramdisk.bin \
	--os_version 13.0.0 --os_patch_level 2025-05 -o out/shim-boot.img
# Host render of the console layout.
gcc -O1 -Wall -o out/host_test console.c host_test.c && ./out/host_test
ls -l out/Image out/shim-boot.img
