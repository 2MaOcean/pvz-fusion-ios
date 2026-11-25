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

// ---- 内存补丁表（mem 数组）：解析地址一次，周期性写值 ----
typedef struct {
    int      active;
    uintptr_t addr;    // 最终地址（解析后缓存）
    int      size;
    long     value;
} MemResolved;

static MemResolved g_mem[MEM_MAX_PATCHES];
static int g_mem_count = 0;
static int g_mem_resolved = 0;

static uintptr_t mem_resolve_patch(const MemPatch *mp, const char *image_keyword) {
    uintptr_t addr = 0;
    if (mp->symbol[0]) addr = (uintptr_t)mem_find_symbol(image_keyword, mp->symbol);
    if (!addr && mp->offset) addr = mem_image_base(image_keyword) + mp->offset;
    if (!addr) return 0;
    if (mp->deref) {
        uintptr_t obj = mem_deref(addr);
        if (!obj) return 0;
        addr = obj + (mp->field_offset ? mp->field_offset : 0);
    }
    return addr;
}

void mod_mem_init(TweakConfig *cfg, const char *image_keyword) {
    g_mem_count = 0;
    g_mem_resolved = 1;
    for (int i = 0; i < cfg->mem_count && i < MEM_MAX_PATCHES; i++) {
        MemPatch *mp = &cfg->mem[i];
        if (!mp->enabled) continue;
        uintptr_t a = mem_resolve_patch(mp, image_keyword);
        TWEAK_LOG("mem[%d] %s: addr=%p size=%d value=%ld %s",
                  i, mp->name, (void *)a, mp->size, mp->value,
                  a ? "OK" : "(未解析,稍后重试)");
        if (a) {
            g_mem[g_mem_count].active = 1;
            g_mem[g_mem_count].addr = a;
            g_mem[g_mem_count].size = mp->size > 0 && mp->size <= 8 ? mp->size : 8;
            g_mem[g_mem_count].value = mp->value;
            g_mem_count++;
        }
    }
    TWEAK_LOG("mem: %d patches active", g_mem_count);
}

void mod_mem_tick(TweakConfig *cfg, const char *image_keyword) {
    if (!cfg->mem_count) return;
    if (!g_mem_resolved) mod_mem_init(cfg, image_keyword);
    for (int i = 0; i < g_mem_count; i++) {
        MemResolved *m = &g_mem[i];
        if (!m->active || !m->addr) continue;
        long long v = (long long)m->value;
        mem_patch(m->addr, &v, (size_t)m->size);
    }
}
