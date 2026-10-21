#!/bin/bash
# Build an iOS root image with the boot-time test runner (no network service):
# clone IN.img (APFS), copy DIR/* to /usr/local/sweettest/ and add a run-once
# launchd job for /usr/local/sweettest/sweettest, which runs the tests named by
# the kernel boot-arg sweettest=NAME[,NAME] (output via /dev/null -> xnu 0048
# sweetnull -> console -> USB live log).
#   add-sweettest.sh IN.img OUT.img DIR
set -euo pipefail
in=$1 out=$2 dir=$3
[ -e "$out" ] && { echo "refusing to overwrite $out" >&2; exit 1; }
mnt=$(mktemp -d "${TMPDIR:-/tmp}/iosroot.XXXXXX")
cp -c "$in" "$out"
dev=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nobrowse -mountpoint "$mnt" "$out" | awk 'NR==1{print $1}')
trap 'hdiutil detach "$dev" >/dev/null 2>&1 || true' EXIT
mkdir -p "$mnt/usr/local/sweettest"
cp "$dir"/* "$mnt/usr/local/sweettest/"
chmod 755 "$mnt/usr/local/sweettest"/*
if [ -e "$dir/libvulkan.dylib" ]; then        # mini-loader for ash's dlopen("libvulkan.dylib")
	mv "$mnt/usr/local/sweettest/libvulkan.dylib" "$mnt/usr/lib/libvulkan.dylib"
	chmod 755 "$mnt/usr/lib/libvulkan.dylib"
fi
python3 - "$mnt/System/Library/xpc/launchd.plist" <<'EOF'
import plistlib, sys
path = sys.argv[1]
raw = open(path, 'rb').read()
fmt = plistlib.FMT_BINARY if raw.startswith(b'bplist') else plistlib.FMT_XML
p = plistlib.loads(raw)
p['LaunchDaemons']['/System/Library/LaunchDaemons/com.sweet.sweettest.plist'] = {
    'Label': 'com.sweet.sweettest',
    'ProgramArguments': ['/usr/local/sweettest/sweettest'],
    'RunAtLoad': True,
    'KeepAlive': False,
    'LaunchOnlyOnce': True,
    'UserName': 'root',
    'StandardOutPath': '/dev/null',
    'StandardErrorPath': '/dev/null',
}
open(path, 'wb').write(plistlib.dumps(p, fmt=fmt))
print('launchd.plist: %d daemons, added com.sweet.sweettest' % len(p['LaunchDaemons']))
EOF
ls -l "$mnt/usr/local/sweettest"
sync
hdiutil detach "$dev" >/dev/null
trap - EXIT
echo "OK: $out (sparse image: tools/img2simg.py)"
