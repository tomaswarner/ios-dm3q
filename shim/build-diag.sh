#!/bin/bash
# Build the DM3Q (S23 Ultra) diagnostic shim as a boot header v4 image plus
# Odin/samloader tars. No XNU: diag.c replaces shim.c.
# Samsung has no `fastboot boot`: the image is flashed to the boot partition
# and the stock boot.img (same build as the phone) is flashed back afterwards.
# The DTB stays in the stock vendor_boot; boot v4 carries only the kernel.
set -euo pipefail
cd "$(dirname "$0")"
CC=aarch64-elf-gcc
OBJCOPY=aarch64-elf-objcopy
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
MKBOOTIMG=../../tools/mkbootimg/mkbootimg.py
AVBTOOL=../../tools/avb/avbtool.py
# Header fields of the phone's own build (S918BXXS8DYDC, patch 2025-05)
OS_VERSION=13.0.0
OS_PATCH=2025-05
mkdir -p out/diag
for f in start.S diag.c console.c; do
	$CC $CFLAGS -c $f -o out/diag/${f%.*}.o
done
abs=$(aarch64-elf-readelf -r out/diag/diag.o out/diag/console.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/diag/start.o out/diag/diag.o out/diag/console.o -lgcc -o out/diag/diag.elf
$OBJCOPY -O binary out/diag/diag.elf out/diag/Image
size=$(aarch64-elf-nm out/diag/diag.elf | awk '$3=="_end"{print $1}')
truncate -s $((16#$size)) out/diag/Image
# Empty newc cpio, as shim/build.sh does (ABL dislikes a zero-size ramdisk on some builds)
(cd out/diag && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
python3 $MKBOOTIMG --header_version 4 --kernel out/diag/Image --ramdisk out/diag/ramdisk.bin \
	--os_version $OS_VERSION --os_patch_level $OS_PATCH -o out/diag/boot.img
# AVB off for boot (vbmeta flags 3 = verity + verification disabled), as Magisk does
python3 $AVBTOOL make_vbmeta_image --flags 3 --padding_size 4096 --output out/diag/vbmeta.img
# Odin wants ustar tars with the partition image names
(cd out/diag && tar --format ustar -cf dm3q-diag-boot.tar boot.img \
	&& tar --format ustar -cf dm3q-vbmeta-disabled.tar vbmeta.img)
ls -l out/diag/Image out/diag/boot.img out/diag/*.tar
