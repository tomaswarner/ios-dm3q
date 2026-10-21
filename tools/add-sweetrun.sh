#!/bin/bash
# Add the sweetrun developer test runner (tools/sweettest/sweetrun.c) to an iOS
# root image: /usr/local/sweettest/sweetrun + a KeepAlive launchd job. It listens
# only on the USB Ethernet address 192.168.7.2:5141 and needs the token from
# ~/.sweetrun-token (created here on first use, kept out of git).
#   add-sweetrun.sh IN.img OUT.img SWEETRUN_BINARY
set -euo pipefail
in=$1 out=$2 bin=$3
[ -e "$out" ] && { echo "refusing to overwrite $out" >&2; exit 1; }
# launchd panics on a non-platform daemon and sweet_amfi only makes *signed* root-fs code
# platform; the iOS clang link does not sign (codesign -f -s - FILE).
for f in "$bin"; do
	if file -b "$f" | grep -q Mach-O && ! codesign -v "$f" 2>/dev/null; then
		echo "unsigned Mach-O: $f (codesign -f -s - $f)" >&2; exit 1
	fi
done
tok=~/.sweetrun-token
[ -s $tok ] || { (umask 077; openssl rand -hex 16 > $tok); }
mnt=$(mktemp -d "${TMPDIR:-/tmp}/iosroot.XXXXXX")
cp -c "$in" "$out"
dev=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nobrowse -mountpoint "$mnt" "$out" | awk 'NR==1{print $1}')
trap 'hdiutil detach "$dev" >/dev/null 2>&1 || true' EXIT
mkdir -p "$mnt/usr/local/sweettest"
cp "$bin" "$mnt/usr/local/sweettest/sweetrun"
chmod 755 "$mnt/usr/local/sweettest/sweetrun"
cp $tok "$mnt/usr/local/sweettest/sweetrun.token"
chmod 600 "$mnt/usr/local/sweettest/sweetrun.token"
python3 - "$mnt/System/Library/xpc/launchd.plist" <<'EOF'
import plistlib, sys
path = sys.argv[1]
raw = open(path, 'rb').read()
fmt = plistlib.FMT_BINARY if raw.startswith(b'bplist') else plistlib.FMT_XML
p = plistlib.loads(raw)
p['LaunchDaemons']['/System/Library/LaunchDaemons/com.sweet.sweetrun.plist'] = {
    'Label': 'com.sweet.sweetrun',
    'ProgramArguments': ['/usr/local/sweettest/sweetrun'],
    'RunAtLoad': True,
    'KeepAlive': True,
    'UserName': 'root',
    'StandardOutPath': '/dev/null',
    'StandardErrorPath': '/dev/null',
}
open(path, 'wb').write(plistlib.dumps(p, fmt=fmt))
print('launchd.plist: %d daemons, added com.sweet.sweetrun' % len(p['LaunchDaemons']))
EOF
ls -l "$mnt/usr/local/sweettest"
sync
hdiutil detach "$dev" >/dev/null
trap - EXIT
echo "OK: $out (sparse image: tools/img2simg.py)"
