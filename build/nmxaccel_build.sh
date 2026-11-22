#!/bin/bash
set -uo pipefail
# G15 fork: nvrm-accel.cpp built with -DNMX_ACCEL as NMXAccel.kext (the Metal accelerator for the GPUs NMXGPU drives),
# from the IrisXe fork (zep245) build/nmxaccel_build.sh; same flags as accel_build.sh otherwise.
SRC=$1; OUT=$2; mkdir -p "$OUT/NMXAccel.kext/Contents/MacOS"
OGKM=${OGKM:-$HOME/ogkm610}; RE=${RE:-$SRC/accel/re}   # the reconstructed IOGraphicsAccelerator2 / IOAccelerator headers ship in the repo
SDK=$(xcrun --show-sdk-path); KHDR="$SDK/System/Library/Frameworks/Kernel.framework/Headers"; NV="$OGKM/src/nvidia"; KMS="$OGKM/src/nvidia-modeset"
RMINC=( -I"$NV/arch/nvalloc/unix/include" -I"$NV/arch/nvalloc/common/inc" -I"$NV/arch/nvalloc/common/inc/gsp"
  -I"$OGKM/src/common/sdk/nvidia/inc" -I"$OGKM/src/common/sdk/nvidia/inc/hw" -I"$OGKM/src/common/inc"
  -I"$OGKM/src/common/shared/inc" -I"$NV/inc" -I"$NV/inc/os" -I"$NV/inc/kernel" -I"$NV/kernel/inc" -I"$NV/interface"
  -I"$OGKM/src/common/nvlink/interface" -I"$OGKM/src/common/inc/swref" -I"$OGKM/src/common/inc/swref/published"
  -I"$NV/inc/libraries" -I"$NV/src/libraries" -I"$NV/generated" -I"$NV/src/mm/uvm/interface"
  -I"$OGKM/src/common/uproc/os/libos-v2.0.0/include" -I"$OGKM/src/common/uproc/os/common/include"
  -I"$OGKM/src/common/inc/displayport" -I"$OGKM/src/common/nvlink/inband/interface" )
KMSINC=( -I"$KMS/os-interface/include" -I"$KMS/kapi/interface" -I"$KMS/kapi/include" -I"$OGKM/src/common/unix/nvidia-push/interface" -I"$KMS/interface" -I"$KMS/include"
         -I"$OGKM/src/common/unix/common/inc" -I"$OGKM/src/common/modeset" -I"$OGKM/src/common/unix/common/utils/interface" )
RMDEFS=(); while read -r d; do RMDEFS+=("$d"); done < <(head -1 "$NV/_out/Darwin_x86_64/compile_cmds.sh" | tr ' ' '\n' | grep -E '^-D' | sed -e 's/"//g' | grep -v -E '^-D(NVRM|_LANGUAGE_C)$')
CFLAGS=( -arch x86_64 -fapple-kext -mkernel -nostdinc -I"$KHDR" -I"$SRC" "${RMINC[@]}" "${KMSINC[@]}"
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -DNMX_ACCEL "${RMDEFS[@]}" ${NM_TAHOE:+-DNM_TAHOE}   # NM_TAHOE=1: the macOS 26 build
  -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common
  -Wall -Wno-unused-parameter -Wno-inconsistent-missing-override -Wno-unused-function -O2 )
xcrun clang++ "${CFLAGS[@]}" -I"$SRC/accel" -I"$SRC/accel/iofam" -I"$RE" -c "$SRC/accel/nvrm-accel.cpp" -o "$OUT/nmx-accel.o" 2>"$OUT/nmxaccel.err" || { grep error "$OUT/nmxaccel.err" | head; exit 1; }
bash "$(dirname "$0")/warn_ceiling.sh" nmxaccel "$(grep -c ': warning:' "$OUT/nmxaccel.err")" "$OUT/nmxaccel.err" || exit 4
xcrun clang++ -arch x86_64 -fapple-kext -nostdlib -Xlinker -kext -lkmodc++ -lkmod -lcc_kext "$OUT/nmx-accel.o" -o "$OUT/NMXAccel.kext/Contents/MacOS/NMXAccel" 2>"$OUT/nmxaccelld.err" || { cat "$OUT/nmxaccelld.err" | head; exit 1; }
bash "$(dirname "$0")/warn_ceiling.sh" nmxaccel-link "$(grep -c 'warning' "$OUT/nmxaccelld.err")" "$OUT/nmxaccelld.err" || exit 4
cp "$SRC/accel/nmx/Info.plist" "$OUT/NMXAccel.kext/Contents/Info.plist"
echo "NMXAccel $(md5 -q "$OUT/NMXAccel.kext/Contents/MacOS/NMXAccel") $(stat -f %z "$OUT/NMXAccel.kext/Contents/MacOS/NMXAccel") B  ($(xcrun clang++ --version | head -1))"
