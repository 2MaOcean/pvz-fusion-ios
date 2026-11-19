// unity_tweak.c — Unity il2cpp 运行时反射修改器（版本无关）
// 原理：dlopen UnityFramework → 拿 il2cpp_* C API → 运行时定位类/静态字段 → 周期性写值
// 无需逆向偏移；对任何 il2cpp 构建的 PvZ Unity 版基座通用。
#include "tweak_main.h"
#include "pvz_tweak.h"
#include "mach_compat.h"

#include <string.h>

// ---- il2cpp 公开 C API（Apple 公开导出于 UnityFramework） ----
static void *(*p_domain_get)(void);
static void **(*p_domain_get_assemblies)(void *, size_t *);
static const char *(*p_image_get_name)(void *);
static size_t (*p_image_get_class_count)(void *);
static void *(*p_class_from_name)(void *, const char *, const char *);
static void *(*p_class_get_field_from_name)(void *, const char *);
static void (*p_field_static_get_value)(void *, void *);
static void (*p_field_static_set_value)(void *, void *, void *);

static int api_ready(void) {
    return p_domain_get && p_domain_get_assemblies && p_class_from_name &&
           p_class_get_field_from_name && p_field_static_set_value;
}

static int resolve_api(void) {
    void *h = dlopen("@r/UnityFramework.framework/UnityFramework", RTLD_LAZY | RTLD_GLOBAL);
    if (!h) h = dlopen("UnityFramework", RTLD_LAZY | RTLD_GLOBAL);
    if (!h) h = dlopen("@executable_path/Frameworks/UnityFramework.framework/UnityFramework", RTLD_LAZY | RTLD_GLOBAL);
    if (!h) { TWEAK_LOG("unity: dlopen UnityFramework failed"); return 0; }
    p_domain_get = dlsym(h, "il2cpp_domain_get");
    p_domain_get_assemblies = dlsym(h, "il2cpp_domain_get_assemblies");
    p_image_get_name = dlsym(h, "il2cpp_image_get_name");
    p_image_get_class_count = dlsym(h, "il2cpp_image_get_class_count");
    p_class_from_name = dlsym(h, "il2cpp_class_from_name");
    p_class_get_field_from_name = dlsym(h, "il2cpp_class_get_field_from_name");
    p_field_static_get_value = dlsym(h, "il2cpp_field_static_get_value");
    p_field_static_set_value = dlsym(h, "il2cpp_field_static_set_value");
    if (!api_ready()) { TWEAK_LOG("unity: il2cpp api incomplete"); return 0; }
    TWEAK_LOG("unity: il2cpp api resolved");
    return 1;
}

static void *find_image_game_csharp(void *domain) {
    size_t n = 0;
    void **asms = p_domain_get_assemblies(domain, &n);
    if (!asms) return NULL;
    for (size_t i = 0; i < n; i++) {
        void *img = asms[i];
        if (!img) continue;
        const char *name = p_image_get_name ? p_image_get_name(img) : NULL;
        if (name && strstr(name, "Assembly-CSharp")) return img;
    }
    return NULL;
}

static long parse_value(const char *s) {
    return s ? strtol(s, NULL, 0) : 0;
}

void *unity_worker(void *arg) {
    TweakConfig *cfg = arg;
    // 等 il2cpp 运行时就绪（Unity 初始化较慢）
    for (int i = 0; i < 1800 && !api_ready(); i++) { // 最多 180s
        usleep(100000);
        if (!p_domain_get && !resolve_api()) continue;
    }
    if (!resolve_api()) { free(cfg); return NULL; }

    void *image = NULL;
    for (int i = 0; i < 1800 && !image; i++) {
        void *domain = p_domain_get();
        if (domain) image = find_image_game_csharp(domain);
        if (!image) usleep(100000);
    }
    if (!image) { TWEAK_LOG("unity: Assembly-CSharp image not found"); free(cfg); return NULL; }
    TWEAK_LOG("unity: Assembly-CSharp found");

    // 逐功能解析字段并锁定
    for (int fi = 0; fi < UNITY_MAX_FEATURES; fi++) {
        UnityFeature *uf = &cfg->unity[fi];
        if (!uf->klass[0] || !uf->field[0]) continue;
        void *klass = p_class_from_name(image, uf->ns[0] ? uf->ns : "", uf->klass);
        if (!klass) { TWEAK_LOG("unity: class %s not found", uf->klass); continue; }
        void *field = p_class_get_field_from_name(klass, uf->field);
        if (!field) { TWEAK_LOG("unity: field %s.%s not found", uf->klass, uf->field); continue; }

        long target = parse_value(uf->value_s);
        int is_float = strcmp(uf->type, "float") == 0;
        int is_bool = strcmp(uf->type, "bool") == 0;
        TWEAK_LOG("unity: locking %s.%s = %ld (%s)", uf->klass, uf->field, target,
                  is_float ? "float" : is_bool ? "bool" : "int");

        for (;;) {
            if (is_float) { float v = (float)target; p_field_static_set_value(field, &v, &v); }
            else if (is_bool) { unsigned char v = target ? 1 : 0; p_field_static_set_value(field, &v, &v); }
            else { int v = (int)target; p_field_static_set_value(field, &v, &v); }
            int pm = uf->poll_ms > 0 ? uf->poll_ms : (cfg->poll_ms > 0 ? cfg->poll_ms : 300);
            usleep(pm * 1000);
        }
    }
    TWEAK_LOG("unity: no active features, worker exit");
    free(cfg);
    return NULL;
}
