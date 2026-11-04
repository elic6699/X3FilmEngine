#!/bin/bash
# build-host.sh -- build spektralab-host on macOS (the child process X3Lumina runs).
#
#   engine/build-host.sh [output]           default: engine/build/spektralab-host
#
# Prerequisites, in this order:
#   engine/setup-libraw.sh     LibRaw 0.22.2 into <repo>/build/deps
#   engine/build.sh lib        engine static library + spektrafilm.metallib
#
# Why this is a separate script: build.sh covers the engine library and the
# Metal kernels, and CMakeLists.txt covers Linux/Windows (Vulkan). Neither
# builds the macOS host process, so its link line lives here.
set -euo pipefail

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$here/.." && pwd)
out=${1:-"$here/build/spektralab-host"}

CXX=${CXX:-clang++}
deps=${SPEKTRALAB_DEPS_DIR:-"$repo/build/deps"}
libraw="$deps/LibRaw-0.22.2"
engine_lib="$here/build/libspektrafilm_engine.a"

# No -fmetal-math-* here: those flags belong to the Metal compiler, not to
# clang++. The kernels are already compiled with safe math by
# `engine/build.sh metallib` (checked above), and the host only loads that
# metallib at run time.
# Same floor as the engine (AppBundle LSMinimumSystemVersion).
target_flags=(-mmacosx-version-min=15.0)

[[ -f "$engine_lib" ]] || {
  echo "no $engine_lib -- run engine/build.sh lib first" >&2; exit 1; }
[[ -f "$libraw/lib/libraw.a" ]] || {
  echo "no $libraw/lib/libraw.a -- run engine/setup-libraw.sh first" >&2; exit 1; }
[[ -f "$here/build/spektrafilm.metallib" ]] || {
  echo "no spektrafilm.metallib -- run engine/build.sh metallib first" >&2; exit 1; }

includes=(
  -I"$here/host"
  -I"$here/include"
  -I"$here/src"
  -I"$here/src/core"
  -I"$here/third_party/stb"
  -I"$here/third_party/metal-cpp"
  -I"$libraw"
)
# Foundation/QuartzCore/CoreText/CoreGraphics/Accelerate: the same set the
# engine links (edge print, date stamp, vImage blur).
frameworks=(
  -framework Metal -framework Foundation -framework QuartzCore
  -framework CoreText -framework CoreGraphics -framework Accelerate
)

shopt -s nullglob
sources=("$here"/host/*.cpp "$here"/src/io/*.cpp)

echo "==> $CXX ${sources[*]##*/}"
"$CXX" -std=c++20 -O2 "${target_flags[@]}" "${includes[@]}" \
  "${sources[@]}" \
  "$engine_lib" "$libraw/lib/libraw.a" \
  "${frameworks[@]}" -lz \
  -o "$out"

echo "==> $out"
"$out" --help 2>&1 | head -1
