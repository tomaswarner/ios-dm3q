#!/usr/bin/env python3
"""llvm-config for the static iOS arm64 LLVM in third_party/llvm-ios.

The iOS build cannot run on the Mac, so this answers with Homebrew's
llvm-config of the same version (23.1.4) and rewrites it for the iOS tree:
its paths, only the static libraries that exist there (AArch64 target only),
no system libraries (built without zlib/zstd/libxml2/terminfo) and no RTTI.
Used by meson through tools/mesa-ios-arm64.cross ([binaries] llvm-config).
"""
import os
import subprocess
import sys

HOST = '/opt/homebrew/opt/llvm/bin/llvm-config'
IOS = '/Users/tomaswarner/hios/third_party/llvm-ios'   # symlink: no space in the path (meson splits flags)
LIBDIR = os.path.join(IOS, 'lib')
have = {f[3:-2] for f in os.listdir(LIBDIR) if f.startswith('libLLVM') and f.endswith('.a')}

args = sys.argv[1:]
fixed = {
    '--prefix': IOS, '--libdir': LIBDIR, '--includedir': os.path.join(IOS, 'include'),
    '--bindir': os.path.join(IOS, 'bin'), '--targets-built': 'AArch64', '--host-target': 'arm64-apple-ios16.0',
    '--has-rtti': 'NO', '--shared-mode': 'static', '--system-libs': '', '--assertion-mode': 'OFF',
    '--build-mode': 'Release',
}
if len(args) == 1 and args[0] in fixed:
    print(fixed[args[0]])
    sys.exit(0)
if '--system-libs' in args and all(a in ('--system-libs', '--link-static') for a in args):
    print('')
    sys.exit(0)

host_args = [a for a in args if a != '--link-shared'] + ([] if '--link-static' in args else [])
out = subprocess.run([HOST] + host_args, capture_output=True, text=True)
if out.returncode:
    sys.stderr.write(out.stderr)
    sys.exit(out.returncode)
text = out.stdout
host_prefix = subprocess.run([HOST, '--prefix'], capture_output=True, text=True).stdout.strip()
text = text.replace(os.path.realpath(host_prefix), IOS).replace(host_prefix, IOS)

words = []
for w in text.split():
    if w.startswith('-lLLVM'):
        if w[2:] in have:
            words.append(w)
        continue
    if w.endswith('.a') and os.path.basename(w).startswith('libLLVM'):
        if os.path.basename(w)[3:-2] in have:
            words.append(w)
        continue
    if w in ('-lz', '-lzstd', '-lxml2', '-lcurses', '-lncurses', '-ltinfo', '-ledit', '-lffi', '-lPolly', '-lPollyISL'):
        continue
    if w == '-frtti':
        continue
    if w.startswith('/') and not w.startswith(IOS) or w.endswith('.dylib') or w in ('-lz3',):
        continue                        # anything of the host LLVM's (z3, ...) is built for macOS
    words.append(w)
if '--components' in args:
    print(' '.join(words))
else:
    print(' '.join(words))
