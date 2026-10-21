#!/bin/sh
# Builds SweetMetal.dylib (the bundle's code, loaded by dlopen for now) and mtl_probe for iOS 16,
# ad-hoc signed (sweet_amfi makes only signed root-fs code platform), into OUT.
set -e
cd "$(dirname "$0")/.."
OUT=${OUT:-../../third_party/sweettest-ios}
IPHONEOS_DEPLOYMENT_TARGET=16.0 cargo build --release --target aarch64-apple-ios
CC="xcrun -sdk iphoneos clang -target arm64-apple-ios16.0 -fobjc-arc -Iinclude"
$CC -Wno-objc-protocol-property-synthesis -Wno-protocol -Wno-objc-protocol-method-implementation -dynamiclib \
	-install_name /usr/local/sweettest/SweetMetal.dylib objc/SweetMetal.m objc/SweetMetalRender.m \
	target/aarch64-apple-ios/release/libsweetmetal.a -framework Foundation -framework Metal \
	-framework IOSurface -o "$OUT/SweetMetal.dylib"
# The bundle's executable: the same code with the bundle's install name
$CC -Wno-objc-protocol-property-synthesis -Wno-protocol -Wno-objc-protocol-method-implementation -dynamiclib \
	-install_name /System/Library/Extensions/SweetMetal.bundle/SweetMetal objc/SweetMetal.m objc/SweetMetalRender.m \
	target/aarch64-apple-ios/release/libsweetmetal.a -framework Foundation -framework Metal \
	-framework IOSurface -o "$OUT/SweetMetal"
cp objc/Info.plist "$OUT/Info.plist"
xcrun -sdk iphoneos clang -target arm64-apple-ios16.0 -O2 objc/sm_install.c -o "$OUT/sm_install"
$CC objc/mtl_probe.m -framework Foundation -framework Metal -framework IOSurface -o "$OUT/mtl_probe"
xcrun -sdk iphoneos metal -target air64-apple-ios16.0 -o "$OUT/sweetmetal_test.metallib" tests/sweetmetal_test.metal
codesign -f -s - -i SweetMetal "$OUT/SweetMetal.dylib"
codesign -f -s - -i mtl_probe "$OUT/mtl_probe"
codesign -f -s - -i com.sweet.SweetMetal "$OUT/SweetMetal"
codesign -f -s - -i sm_install "$OUT/sm_install"
