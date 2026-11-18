#!/bin/bash
# make_ipa.sh — 把修改器 dylib 注入基座 ipa 并重签（macOS 运行）
# 用法: ./make_ipa.sh <base.ipa> <PvzFusionTweak.dylib> <out.ipa>
set -euo pipefail

BASE=${1:?用法: make_ipa.sh <base.ipa> <dylib> <out.ipa>}
DYLIB=${2:?缺 dylib}
OUT=${3:?缺输出路径}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
unzip -q "$BASE" -d "$WORK"

APP=$(find "$WORK/Payload" -maxdepth 1 -name "*.app" | head -1)
[ -n "$APP" ] || { echo "错误: ipa 里找不到 .app"; exit 1; }
NAME=$(basename "$APP" .app)
BIN=$(defaults read "$APP/Info" CFBundleExecutable)

cp "$DYLIB" "$APP/PvzFusionTweak.dylib"

# 用 insert_dylib 改主二进制 load commands
INSDY=$(command -v insert_dylib || true)
if [ -z "$INSDY" ]; then
  echo "提示: 未安装 insert_dylib，尝试 brew install insert_dylib ..."
  brew install insert_dylib || true
  INSDY=$(command -v insert_dylib || true)
fi
if [ -n "$INSDY" ]; then
  "$INSDY" --weak --inplace "@executable_path/PvzFusionTweak.dylib" "$APP/$BIN"
else
  echo "错误: 需要 insert_dylib（brew install insert_dylib）来修改 Mach-O"
  exit 1
fi

# 重签：TrollStore 接受 ldid 伪签；侧载建议再用 Sideloadly/AltStore 换个人证书
if command -v ldid >/dev/null; then
  ldid -S "$APP/$BIN"
  ldid -S "$APP/PvzFusionTweak.dylib"
else
  codesign --force --sign - --deep "$APP"
fi

mkdir -p "$(dirname "$OUT")"
(cd "$WORK" && zip -qr "$OLDPWD/$OUT" Payload)
echo "完成: $OUT"
