# PVZ Fusion iOS 修改器 dylib —— macOS + Xcode 构建
# 产物: build/PvzFusionTweak.dylib

SDK      := $(shell xcrun --sdk iphoneos --show-sdk-path 2>/dev/null)
CC       := xcrun -sdk iphoneos clang
ARCH     := -arch arm64 -arch arm64e
MINIOS   := -miphoneos-version-min=13.0
CFLAGS   := $(ARCH) $(MINIOS) -isysroot $(SDK) -O2 -Wall -dynamiclib \
            -Iinclude -fvisibility=hidden -F$(SDK)/System/Library/Frameworks
LDLIBS   := -framework CoreFoundation
SIGN     := $(shell command -v ldid 2>/dev/null)

BUILD    := build
TARGET   := $(BUILD)/PvzFusionTweak.dylib
SRCS     := src/tweak_main.c src/hook.c src/config.c src/modules.c
HDRS     := include/pvz_tweak.h include/tweak_main.h

.PHONY: all clean install-ldid

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS) | $(BUILD)
	@echo "== 检查 iOS SDK =="
	@test -n "$(SDK)" || { echo "错误: 未找到 iphoneos SDK，请安装 Xcode"; exit 1; }
	@echo "SDK: $(SDK)"
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)
ifdef SIGN
	@echo "== ldid 伪签（非越狱可加载） =="
	$(SIGN) -S $@
else
	@echo "== 用 codesign ad-hoc 签名 =="
	codesign --force --sign - $@
endif
	@echo "完成: $@"

$(BUILD):
	mkdir -p $(BUILD)

install-ldid:
	brew install ldid

clean:
	rm -rf $(BUILD)
