#!/bin/bash
# pair.sh FRAGMENT -> first vertex function that compiles with it (or FAIL + last reason)
T="/Users/tomaswarner/Desktop/hackios s23/third_party/metal2vulkan"; B="$T/target/release/metal2vulkan"
P="$T/crates/metal2vulkan-validation/profiles/lavapipe.json"; LIB=System_Library_Frameworks_QuartzCore.framework_default.metallib
frag=$1; last=""
for v in $(cat vertex.txt); do
  r=$("$B" $LIB /tmp/m2v-$frag.spv --profile "$P" --function $v --fragment $frag --color-format 0:BGRA8Unorm --vertex-descriptor vd.json --constant 0:bool:false --constant 1:bool:false --no-spirv-val 2>&1 | tail -1)
  case "$r" in *PASS*) echo "PASS|$frag|$v"; exit;; esac
  last=$r
done
echo "FAIL|$frag|$last"
