// modules.c — 阳光 / 金币锁定 + 无冷却补丁
#include "pvz_tweak.h"
#include <string.h>

// 定位目标地址：
//   symbol 优先（dlsym / 导出表）→ 得到“全局对象或变量”
//   否则 image_base + offset（静态布局，适配 TrollStore 全功能环境）
//   field_offset != 0 时对地址解引用一次再偏移（堆对象字段）
static uintptr_t resolve_target(const TweakFeature *f, const char *image_keyword) {
    uintptr_t addr = 0;
    if (f->symbol[0]) {
        addr = (uintptr_t)mem_find_symbol(image_keyword, f->symbol);
        if (addr) TWEAK_LOG("symbol %s -> %p", f->symbol, (void *)addr);
    }
    if (!addr && f->offset) addr = mem_image_base(image_keyword) + f->offset;
    if (!addr) return 0;

    if (f->field_offset) {
        uintptr_t obj = mem_deref(addr);
        if (!obj) return 0;
        addr = obj + f->field_offset;
    }
    return addr;
}

void mod_sun_tick(const TweakFeature *f, const char *image_keyword) {
    if (!f->enabled || !f->value) return;
    static uintptr_t cached = 0;
    if (!cached) cached = resolve_target(f, image_keyword);
    if (!cached) return;
    long v = (long)f->value;
    mem_patch(cached, &v, sizeof(v));
}

void mod_coins_tick(const TweakFeature *f, const char *image_keyword) {
    if (!f->enabled) return;
    static uintptr_t cached = 0;
    if (!cached) cached = resolve_target(f, image_keyword);
    if (!cached) return;
    long v = (long)f->value;
    mem_patch(cached, &v, sizeof(v));
}

// 无冷却：优先 __TEXT 代码补丁（TrollStore / 越狱可用），
// 非越狱下依赖移植版导出的可写函数指针 / vtable（__DATA_CONST）
void mod_cooldown_apply(const TweakFeature *f, const char *image_keyword) {
    if (!f->enabled) return;
    uint8_t bytes[128];
    size_t len = 0;

    if (f->patch_hex[0] && config_parse_patch_hex(f->patch_hex, bytes, sizeof(bytes), &len)) {
        uintptr_t addr = 0;
        if (f->symbol[0]) addr = (uintptr_t)mem_find_symbol(image_keyword, f->symbol);
        if (!addr && f->offset) addr = mem_image_base(image_keyword) + f->offset;
        if (addr && patch_code_bytes(addr, bytes, len)) {
            TWEAK_LOG("cooldown: code patch applied at %p (%zu bytes)", (void *)addr, len);
            return;
        }
        TWEAK_LOG("cooldown: code patch failed (sandboxed?), try ret-stub path");
    }

    if (f->symbol[0]) {
        void *fp = mem_find_symbol(image_keyword, f->symbol);
        if (fp) {
            // 把引擎内的冷却递减入口替换为立即返回（arm64: mov w0,#0; ret）
            static const uint8_t ret_stub[8] = { 0x00,0x00,0x80,0x52, 0xc0,0x03,0x5f,0xd6 };
            if (patch_code_bytes((uintptr_t)fp, ret_stub, sizeof(ret_stub)))
                TWEAK_LOG("cooldown: %s -> ret stub", f->symbol);
        }
    }
}
