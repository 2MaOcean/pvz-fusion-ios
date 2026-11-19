#!/bin/bash
# tweak/build.sh — 通用构建：macOS(Xcode) 或 Linux(zig 交叉) 均可
# 产物: tweak/build/PvzFusionTweak.dylib (arm64 Mach-O)
set -e
cd "$(dirname "$0")"
mkdir -p build

SRCS="src/tweak_main.c src/hook.c src/config.c src/modules.c src/unity_tweak.c"
OUT="build/PvzFusionTweak.dylib"

if command -v xcrun >/dev/null 2>&1 && xcrun --sdk iphoneos --show-sdk-path >/dev/null 2>&1; then
    echo "== macOS Xcode 构建 =="
    SDK=$(xcrun --sdk iphoneos --show-sdk-path)
    xcrun -sdk iphoneos clang -arch arm64 -arch arm64e \
        -miphoneos-version-min=13.0 -isysroot "$SDK" \
        -O2 -dynamiclib -Iinclude -o "$OUT" $SRCS -framework CoreFoundation
    if command -v ldid >/dev/null; then
        ldid -S "$OUT"
    else
        codesign --force --sign - "$OUT"
    fi
else
    echo "== zig 交叉构建（no-SDK, 符号运行时绑定） =="
    ZIG="${ZIG:-python3 -m ziglang}"
    $ZIG cc -target aarch64-macos.11.0.0-none -dynamiclib -O2 \
        -Iinclude -undefined dynamic_lookup -o "$OUT" $SRCS
    if command -v ldid >/dev/null; then
        ldid -S "$OUT"
        echo "已伪签: ldid -S"
    else
        echo "WARN: 未找到 ldid，产物未签名；请在打包阶段（make_ipa / TrollStore）重签"
    fi
fi

ls -la "$OUT"
