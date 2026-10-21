#!/bin/bash
# Make CoreImage report "no GL" on a GPU-less device instead of drawing through an
# EAGL context whose GL dispatch table is all NULL (SpringBoard crash with
# Accessibility > Bold Text: CUIShapeEffectStack sharedCIContext -> CIContext ->
# CI::GLContext::init -> glDisable through a NULL pointer).
#
# CoreImage's _wrapGLIsUsable (iOS 16.7.16 20H392 iPhone10,3, cache VA
# 0x194b31ee0 = dyld_shared_cache_arm64.07 offset 0x1f1ee0) is `mov w0,#1; ret`;
# with 0 there, -[CIContext initWithOptions:] and internalContextWithEAGLContext:
# return nil when there is no Metal device.  Boot with cs_enforcement_disable=1:
# the patched page no longer matches the cache's code-signature page hash.
#
#   patch-ios-root-nogl.sh IN.img OUT.img     (OUT is an APFS clone of IN, then patched)
set -euo pipefail
in=$1 out=$2
sub=System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.07
off=$((0x1f1ee0))
mnt=$(mktemp -d "${TMPDIR:-/tmp}/iosroot.XXXXXX")

[ -e "$out" ] && { echo "refusing to overwrite $out" >&2; exit 1; }
cp -c "$in" "$out"
dev=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nobrowse -mountpoint "$mnt" "$out" | awk 'NR==1{print $1}')
trap 'hdiutil detach "$dev" >/dev/null 2>&1 || true' EXIT
f="$mnt/$sub"
python3 - "$f" "$off" <<'EOF'
import sys
path, off = sys.argv[1], int(sys.argv[2])
want_old = bytes.fromhex("20008052c0035fd6")	# mov w0, #1 ; ret
want_new = bytes.fromhex("00008052c0035fd6")	# mov w0, #0 ; ret
with open(path, "r+b") as f:
    f.seek(off); cur = f.read(8)
    if cur == want_new:
        print("already patched"); sys.exit(0)
    if cur != want_old:
        sys.exit("unexpected bytes %s at %#x: wrong cache build?" % (cur.hex(), off))
    f.seek(off); f.write(want_new[:4]); f.flush()
    f.seek(off); assert f.read(8) == want_new
print("patched _wrapGLIsUsable -> 0 in", path)
EOF
sync
hdiutil detach "$dev" >/dev/null
trap - EXIT
echo "OK: $out (make the sparse image with tools/img2simg.py)"
