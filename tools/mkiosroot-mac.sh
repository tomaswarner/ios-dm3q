#!/bin/bash
# macOS version of tools/mkiosroot.sh: same iOS root (one case-sensitive,
# journaled HFS+ volume "iOSRoot" with the OS root plus both cryptexes),
# built with hdiutil/newfs_hfs instead of apfs-fuse/mkfs.hfsplus. Run as root:
#
#   sudo tools/mkiosroot-mac.sh OS.dmg SYSTEMOS.dmg APPOS.dmg OUT.img [SIZE]   (SIZE default 16g)
#
# OUT.img is a raw volume image (no partition map), flashed as is to userdata,
# with tools/sweet-root-tweaks.py already applied.
# The IPSW's APFS volumes are mounted read-only with owners honoured; files are
# LZBITMAP/decmpfs-compressed and rsync reads them decompressed, so the HFS+
# copy is plain data (the kernel's HFS has no decmpfs).
set -euo pipefail

[[ $# -ge 4 ]] || { sed -n '2,11p' "$0"; exit 2; }
[[ $EUID == 0 ]] || { echo "run as root (sudo): owners and modes must be preserved" >&2; exit 1; }
os=$(realpath "$1") sysos=$(realpath "$2") appos=$(realpath "$3") out=$4 size=${5:-16g}
[[ ! -e $out ]] || { echo "refusing to overwrite $out" >&2; exit 1; }
RSYNC=/opt/homebrew/bin/rsync	# rsync 3: macOS' openrsync lacks reliable -H
[[ -x $RSYNC ]] || { echo "need rsync 3: brew install rsync" >&2; exit 1; }

work=$(mktemp -d /private/tmp/mkiosroot.XXXXXX)
devs=()
cleanup() {
	for d in "${devs[@]}"; do
		hdiutil detach -quiet "$d" 2>/dev/null || hdiutil detach -force -quiet "$d" 2>/dev/null || true
	done
	rmdir "$work"/* "$work" 2>/dev/null || true
}
trap cleanup EXIT

# Attach read-only with owners honoured; the cryptexes are raw APFS, the OS
# image is UDIF. hdiutil mounts the volume of the synthesized APFS container.
attach() {
	local img=$1 name=$2 class=()
	[[ $3 == raw ]] && class=(-imagekey diskimage-class=CRawDiskImage)
	mkdir "$work/$name"
	local dev
	dev=$(hdiutil attach -nobrowse -readonly -noverify -owners on -mountpoint "$work/$name" \
		"${class[@]}" "$img" | awk 'NR==1{print $1}')
	devs+=("$dev")
	mount | grep -q " on $work/$name " || { echo "$img: no volume mounted" >&2; exit 1; }
	echo "$name: $img ($dev)"
}
attach "$os" os udif
attach "$sysos" sysos raw
attach "$appos" appos raw

# Raw image file -> block device -> case-sensitive journaled HFS+
mkfile -n "$size" "$out"
rdev=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$out" | awk 'NR==1{print $1}')
devs=("$rdev" "${devs[@]}")
newfs_hfs -s -J -v iOSRoot "${rdev/disk/rdisk}"
mkdir "$work/root"
mount -t hfs -o owners,nobrowse "$rdev" "$work/root"

copy() {
	echo "copying $1 -> ${2#"$work/root"}"
	mkdir -p "$2"
	"$RSYNC" -aH --numeric-ids "$1/" "$2/"
}
copy "$work/os" "$work/root"
copy "$work/sysos" "$work/root/private/preboot/Cryptexes/OS"
copy "$work/appos" "$work/root/private/preboot/Cryptexes/App"
# macOS adds these to every volume it mounts; iOS has no use for them
rm -rf "$work/root/.fseventsd" "$work/root/.Spotlight-V100" "$work/root/.Trashes"

# Shared cache where dyld looks at ignition level 1: hard links, no extra space.
dsc=private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld
mkdir -p "$work/root/System/Library/Caches/com.apple.dyld"
for f in "$work/root/$dsc"/*; do
	ln "$f" "$work/root/System/Library/Caches/com.apple.dyld/${f##*/}"
done

# dyld does not search the cryptexes at ignition level 1: hard-link every
# cryptex file that has no counterpart in the OS root to its root path.
for c in OS App; do
	(cd "$work/root/private/preboot/Cryptexes/$c" && find . ! -type d ! -path './System/Library/Caches/com.apple.dyld/*') |
	while IFS= read -r f; do
		f=${f#./}
		if [[ ! -e "$work/root/$f" && ! -L "$work/root/$f" ]]; then
			mkdir -p "$work/root/$(dirname "$f")"
			src="$work/root/private/preboot/Cryptexes/$c/$f"
			# same as the Linux script: symlinks are copied, files hard-linked
			if [[ -L $src ]]; then cp -P "$src" "$work/root/$f"; else ln "$src" "$work/root/$f"; fi
			echo "linked $c:/$f"
		fi
	done
done

# logd's log stores (owner _logd 272, mode, .migrated marker); see mkiosroot.sh
db="$work/root/private/var/db"
install -d -o 272 -g 272 -m 0751 "$db/diagnostics"
install -d -o 272 -g 272 -m 0755 "$db/uuidtext" "$db/diagnostics/timesync"
for d in diagnostics uuidtext diagnostics/timesync; do
	install -o 272 -g 272 -m 0644 /dev/null "$db/$d/.migrated"
done

# System keybag for the built-in AppleKeyStore (no passcode).
install -d -m 0700 -o 0 -g 0 "$work/root/private/var/keybags"
printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
	'<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	'<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>' \
	> "$work/root/private/var/keybags/systembag.kb"
chown 0:0 "$work/root/private/var/keybags/systembag.kb"
chmod 0600 "$work/root/private/var/keybags/systembag.kb"

# The bring-up changes the Linux flow applies as a separate step (README step 2)
python3 "$(dirname "$0")/sweet-root-tweaks.py" "$work/root"

df -h "$work/root"
umount "$work/root"
fsck_hfs -f -n "${rdev/disk/rdisk}"
