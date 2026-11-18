// pvz_tweak.h — PVZ Fusion iOS 修改器 公共定义
#ifndef PVZ_TWEAK_H
#define PVZ_TWEAK_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define TWEAK_LOG(...) tweak_log("[PvZTweak] " __VA_ARGS__)

// ---- 日志 ----
void tweak_log(const char *fmt, ...);

// ---- 配置（Offsets.plist 驱动） ----
typedef struct {
    bool   enabled;
    long   value;          // 锁定数值
    char   symbol[128];    // 可选：引擎导出符号（全局对象/函数）
    long   offset;         // 相对镜像基址/对象的偏移
    long   field_offset;   // 对象内字段偏移（堆锁定用）
    char   patch_hex[256]; // 可选：TrollStore/越狱下的代码补丁字节
} TweakFeature;

typedef struct {
    char image_keyword[64]; // 镜像名关键字，如 "PvZ"
    int  poll_ms;           // 锁定线程轮询间隔
    TweakFeature sun;       // 阳光锁定
    TweakFeature coins;     // 金币锁定
    TweakFeature cooldown;  // 卡槽无冷却（vtable/补丁）
    TweakFeature no_drop;   // 植物血量不减（可选，默认关）
} TweakConfig;

bool config_load(TweakConfig *cfg);       // 读 bundle 内 Offsets.plist，失败给默认值
bool config_parse_patch_hex(const char *hex, uint8_t *out, size_t max, size_t *out_len);

// ---- 内存 / hook 原语 ----
uintptr_t mem_image_base(const char *keyword);            // 返回镜像头地址（含 slide）
void     *mem_find_symbol(const char *image_keyword,
                          const char *symbol_name);       // dlsym + 导出表扫描
bool      mem_patch(uintptr_t addr, const void *data, size_t len);  // 写内存（自动改页权限）
bool      mem_read(uintptr_t addr, void *out, size_t len);
uintptr_t mem_deref(uintptr_t addr);                      // 读指针
// TrollStore / 越狱：允许写 __TEXT 时返回 true，并打上补丁
bool      patch_code_bytes(uintptr_t addr, const void *data, size_t len);

// ---- 功能模块 ----
void mod_sun_tick(const TweakFeature *f, const char *image_keyword);
void mod_coins_tick(const TweakFeature *f, const char *image_keyword);
void mod_cooldown_apply(const TweakFeature *f, const char *image_keyword);

#endif
