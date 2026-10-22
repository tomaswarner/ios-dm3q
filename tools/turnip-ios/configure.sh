#!/bin/sh
# Configure + build Mesa Turnip (KGSL) for iOS 16 arm64. Run from anywhere.
set -e
export PATH=/opt/homebrew/opt/bison/bin:$HOME/hios/third_party/venv/bin:$PATH   # needs bison>=3 (Xcode's 2.3 fails), mako/pyyaml venv
cd $HOME/hios/third_party/mesa
[ -d build-ios-turnip ] && RECONF=--reconfigure
meson setup $RECONF build-ios-turnip -Dbuildtype=release -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl \
  -Dgallium-drivers= -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false \
  -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled -Dvulkan-layers= -Dtools= -Dzstd=disabled \
  -Dexpat=disabled -Dxmlconfig=disabled -Dcpp_rtti=false -Dzlib=disabled -Dshader-cache=disabled \
  --cross-file=$HOME/hios/port/tools/turnip-ios/mesa-turnip-ios-arm64.cross
# build ONLY the driver (default 'all' also tries libxml2/libarchive subprojects for rnn tools, which fail to link on iOS and are not needed)
ninja -C build-ios-turnip src/freedreno/vulkan/libvulkan_freedreno.dylib
