#!/bin/bash
# Relink the SWEET kernel locally from a CI link kit, replacing some objects: seconds instead of a CI run.
#
#   tools/xnu-relink.sh KIT OUT [NEW.o ...]
#
# KIT   a downloaded xnu-kernel artifact (xnu/out-dm3q-NNN): kernel.development.sweet.link/ (the archive
#       of every object + the link arguments, as Apple's KDK ships it) and relink/ (libfirehose_kernel.a,
#       nonlto.o, symbolsets.plist, all-alias.exp; uploaded by xnu.yml since 0115's workflow change)
# OUT   the kernel to write (stripped, like CI's kernel.development.sweet)
# NEW.o objects that replace archive members of the same base name; tools/xnu-cc.py names them
#       dir__file.c.o (XNU_CC_OBJDIR), which is accepted too (bsd__dev__sweet_kgsl.c.o -> sweet_kgsl.o)
#
# Only for sources that need no regenerated headers (plain .c/.cpp edits; not iig, mig or config
# changes): those still go through CI. No CTF section (dtrace type data) is added.
set -euo pipefail
# The linker must be Xcode 15's (CI's): Xcode 16+'s ld-prime folds __LASTDATA_CONST into __LAST.
export DEVELOPER_DIR=${DEVELOPER_DIR:-$(cd "$(dirname "$0")/../.." && pwd)/xcode/Xcode_15.0.1.app/Contents/Developer}
KIT=$(cd "$1" && pwd)
OUT=$2
shift 2
LINK=$KIT/kernel.development.sweet.link
RL=$KIT/relink
for f in "$LINK/kernel.development.sweet.a" "$LINK/kernel.development.sweet.linkarguments" \
	"$RL/libfirehose_kernel.a" "$RL/nonlto.o" "$RL/symbolsets.plist" "$RL/all-alias.exp"; do
	[ -f "$f" ] || { echo "missing $f (kit from a CI run before the relink kit?)" >&2; exit 1; }
done
W=$(mktemp -d "${TMPDIR:-/tmp}/xnu-relink.XXXXXX")
trap 'rm -rf "$W"' EXIT
mkdir "$W/o" "$W/lib"
(cd "$W/o" && xcrun ar x "$LINK/kernel.development.sweet.a")
for n in "$@"; do
	b=$(basename "$n")
	case $b in   # xnu-cc's dir__file.c.o / dir__file.cpp.o -> the archive's file.o / file.cpo
	*__*.cpp.o) b=${b##*__}; b=${b%.cpp.o}.cpo ;;
	*__*.o) b=${b##*__}; b=${b%.*.o}.o ;;
	esac
	if [ -f "$W/o/$b" ]; then
		echo "replaced $b"
	else
		echo "added $b (new source)"
		echo "$b" >> "$W/added"
	fi
	cp "$n" "$W/o/$b"
done
# archive order, the three objects the Makefile names after the filelist kept last
xcrun ar t "$LINK/kernel.development.sweet.a" | grep -vxE 'version.o|lastkerneldataconst.o|lastkernelconstructor.o|__.SYMDEF.*' |
	sed "s|^|$W/o/|" > "$W/link.filelist"
[ -f "$W/added" ] && sed "s|^|$W/o/|" "$W/added" >> "$W/link.filelist"
cp "$RL/libfirehose_kernel.a" "$W/lib/"
CCKEXT=$(find "$DEVELOPER_DIR/Toolchains/XcodeDefault.xctoolchain/usr/lib/clang" -name libclang_rt.cc_kext.a | head -1)
ln -s "$CCKEXT" "$W/lib/libcc_kext.a"
# CI's arguments without its -L (ours: $W/lib)
# and without -rename_segment,__LASTDATA_CONST,__LAST, which CI's ld ignores ("not yet supported")
# but ours applies (no __LASTDATA_CONST segment then)
ARGS=$(sed -e 's|-L[^ ]*fakeroot/usr/local/lib/kernel||g' -e 's|-Wl,-rename_segment,__LASTDATA_CONST,__LAST||g' \
	"$LINK/kernel.development.sweet.linkarguments")
# shellcheck disable=SC2086
xcrun clang++ $ARGS -L"$W/lib" \
	-Wl,-alias_list,"$RL/all-alias.exp" -Wl,-sectcreate,__LINKINFO,__symbolsets,"$RL/symbolsets.plist" \
	-Wl,-segprot,__LINKINFO,r--,r-- \
	-filelist "$W/link.filelist" "$W/o/version.o" "$W/o/lastkerneldataconst.o" "$W/o/lastkernelconstructor.o" \
	"$RL/nonlto.o" -o "$W/kernel.unstripped" -lcc_kext -lfirehose_kernel 2>&1 |
	grep -v "rename_segment not yet supported\|warning: -new_linker\|ignoring duplicate libraries" || true
[ -f "$W/kernel.unstripped" ] || { echo "link failed" >&2; exit 1; }
xcrun strip -S "$W/kernel.unstripped" -o "$OUT"
ls -l "$OUT"
