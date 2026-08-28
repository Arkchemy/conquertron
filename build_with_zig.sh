#!/bin/sh
# Builds recomp without cmake or a system C++ compiler.
#
# setup_toolchain.sh's cmake path is still the normal route. This exists
# because a machine can easily end up with libcapstone.so.5 and libz.so.1
# present (pulled in as runtime dependencies of something else) but no
# -devel headers, no cmake and no gcc-c++ -- at which point recomp cannot
# be rebuilt at all, and regenerate.sh silently keeps using whatever stale
# binary is sitting in build/. That is not hypothetical: on 2026-08-29 it
# meant a fixed codegen.cpp would have been reverted by the next
# regeneration.
#
# zig ships a complete clang C/C++ frontend, so it can build capstone,
# zlib and recomp itself with no system toolchain at all.
#
# Usage: ZIG=/path/to/zig ./build_with_zig.sh
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-$HOME/devtools/zig/zig}"
DEVTOOLS="${DEVTOOLS:-$HOME/devtools}"
[ -x "$ZIG" ] || { echo "error: zig not found at $ZIG (set ZIG=)" >&2; exit 1; }

mkdir -p "$DEVTOOLS/bin"
printf '#!/bin/sh\nexec %s cc "$@"\n'  "$ZIG" > "$DEVTOOLS/bin/zcc"
printf '#!/bin/sh\nexec %s c++ "$@"\n' "$ZIG" > "$DEVTOOLS/bin/zcxx"
chmod +x "$DEVTOOLS/bin/zcc" "$DEVTOOLS/bin/zcxx"

if [ ! -f "$DEVTOOLS/zinstall/lib/libz.a" ]; then
    echo "building zlib..."
    [ -d "$DEVTOOLS/zlib" ] || git clone --depth 1 --branch v1.3.1 \
        https://github.com/madler/zlib.git "$DEVTOOLS/zlib"
    (cd "$DEVTOOLS/zlib" && CC="$DEVTOOLS/bin/zcc" \
        ./configure --static --prefix="$DEVTOOLS/zinstall" >/dev/null \
        && make -j"$(nproc)" libz.a >/dev/null && make install >/dev/null)
fi

if [ ! -f "$DEVTOOLS/capstone/libcapstone.a" ]; then
    echo "building capstone (powerpc only)..."
    [ -d "$DEVTOOLS/capstone" ] || git clone --depth 1 --branch 5.0.3 \
        https://github.com/capstone-engine/capstone.git "$DEVTOOLS/capstone"
    # `|| true`: only the optional cstool demo fails to link, because it
    # references x86 printers that a powerpc-only build does not compile.
    # libcapstone.a itself is complete by then, which is all recomp needs.
    (cd "$DEVTOOLS/capstone" && make CC="$DEVTOOLS/bin/zcc" \
        CAPSTONE_ARCHS="powerpc" CAPSTONE_STATIC=yes CAPSTONE_SHARED=no \
        -j"$(nproc)" >/dev/null 2>&1 || true)
    [ -f "$DEVTOOLS/capstone/libcapstone.a" ] || {
        echo "error: capstone build produced no libcapstone.a" >&2; exit 1; }
fi

mkdir -p build
for tool in recomp verify_vtable find_synth_addr; do
    case "$tool" in
        recomp) main=src/main.cpp ;;
        *)      main="src/$tool.cpp" ;;
    esac
    echo "building $tool..."
    "$DEVTOOLS/bin/zcxx" -std=c++17 -O2 -DNDEBUG \
        -Iinclude -I"$DEVTOOLS/capstone/include" -I"$DEVTOOLS/zinstall/include" \
        "$main" src/elf_loader.cpp src/disassembler.cpp src/func_recovery.cpp \
        $( [ "$tool" = recomp ] && echo src/codegen.cpp ) \
        "$DEVTOOLS/capstone/libcapstone.a" "$DEVTOOLS/zinstall/lib/libz.a" \
        -o "build/$tool"
done
echo "done: build/recomp"
