// unity_tweak.c — Unity il2cpp 运行时反射修改器 v2(版本无关)
// 原理：dlopen UnityFramework → il2cpp_* C API → 运行时定位类/字段 → 周期性写值
// v2: 多特性并发 tick(修复单特性死循环)、long 类型、实例字段链锁定
//     (GameAPP.board → GameObject.GetComponent(typeof(Board)) → Board.theSun)
#include "tweak_main.h"
#include "pvz_tweak.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <dlfcn.h>
// zig cc no-SDK 下显式声明 dyld(避免 mach_compat 头依赖差异)
extern uint32_t _dyld_image_count(void);
extern const char *_dyld_get_image_name(uint32_t image_index);

// ---- il2cpp 公开 C API ----
static void *(*p_domain_get)(void);
static void **(*p_domain_get_assemblies)(void *, size_t *);
static const char *(*p_image_get_name)(void *);
static size_t (*p_image_get_class_count)(void *);
static void *(*p_class_from_name)(void *, const char *, const char *);
static void *(*p_class_get_field_from_name)(void *, const char *);
static void *(*p_class_get_fields)(void *, void **);
static const char *(*p_field_get_name)(void *);
static int (*p_field_get_flags)(void *);
static void *(*p_field_get_type)(void *);
static const char *(*p_type_get_name)(void *);
static void *(*p_type_get_object)(void *);          // il2cpp_type_get_object → System.Type 实例
static void (*p_free)(void *);
static void (*p_field_static_get_value)(void *, void *);
static void (*p_field_static_set_value)(void *, void *, void *);
static void (*p_field_get_value)(void *, void *, void *);   // (obj, field, out)
static void (*p_field_set_value)(void *, void *, void *);   // (obj, field, value)
static void *(*p_class_get_method_from_name)(void *, const char *, int);
static void *(*p_runtime_invoke)(void *, void *, void **, void **); // (method, obj, params, &exc)
static void *(*p_class_get_type)(void *);           // il2cpp_class_get_type
static void *(*p_array_get)(void *, unsigned long); // il2cpp_array_get

static int api_ready(void) {
    return p_domain_get && p_domain_get_assemblies && p_class_from_name &&
           p_class_get_field_from_name && p_field_static_set_value;
}

static void *must_sym(void *h, const char *n) {
    void *p = dlsym(h, n);
    if (!p) TWEAK_LOG("unity: sym %s missing (optional)", n);
    return p;
}

static int resolve_api(void) {
    void *h = dlopen("@r/UnityFramework.framework/UnityFramework", RTLD_LAZY | RTLD_GLOBAL);
    if (!h) h = dlopen("UnityFramework", RTLD_LAZY | RTLD_GLOBAL);
    if (!h) h = dlopen("@executable_path/Frameworks/UnityFramework.framework/UnityFramework", RTLD_LAZY | RTLD_GLOBAL);
    if (!h) {
        for (uint32_t i = 0; i < _dyld_image_count(); i++) {
            const char *name = _dyld_get_image_name(i);
            if (name && strstr(name, "UnityFramework")) {
                h = dlopen(name, RTLD_LAZY | RTLD_GLOBAL);
                if (h) { TWEAK_LOG("unity: dlopen via image %s", name); break; }
            }
        }
    }
    if (!h) { TWEAK_LOG("unity: dlopen UnityFramework failed"); return 0; }
    p_domain_get = dlsym(h, "il2cpp_domain_get");
    p_domain_get_assemblies = dlsym(h, "il2cpp_domain_get_assemblies");
    p_image_get_name = dlsym(h, "il2cpp_image_get_name");
    p_image_get_class_count = dlsym(h, "il2cpp_image_get_class_count");
    p_class_from_name = dlsym(h, "il2cpp_class_from_name");
    p_class_get_field_from_name = dlsym(h, "il2cpp_class_get_field_from_name");
    p_class_get_fields = dlsym(h, "il2cpp_class_get_fields");
    p_field_get_name = dlsym(h, "il2cpp_field_get_name");
    p_field_get_flags = dlsym(h, "il2cpp_field_get_flags");
    p_field_get_type = dlsym(h, "il2cpp_field_get_type");
    p_type_get_name = dlsym(h, "il2cpp_type_get_name");
    p_free = dlsym(h, "il2cpp_free");
    p_field_static_get_value = dlsym(h, "il2cpp_field_static_get_value");
    p_field_static_set_value = dlsym(h, "il2cpp_field_static_set_value");
    p_field_get_value = dlsym(h, "il2cpp_field_get_value");
    p_field_set_value = dlsym(h, "il2cpp_field_set_value");
    p_class_get_method_from_name = dlsym(h, "il2cpp_class_get_method_from_name");
    p_runtime_invoke = dlsym(h, "il2cpp_runtime_invoke");
    p_type_get_object = dlsym(h, "il2cpp_type_get_object");
    p_class_get_type = dlsym(h, "il2cpp_class_get_type");
    p_array_get = dlsym(h, "il2cpp_array_get");
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

// 字段探测：枚举指定类全部字段（名字/类型/是否静态），写入日志
static void probe_class(void *image, const char *name) {
    if (!p_class_get_fields || !p_field_get_name) { TWEAK_LOG("probe: api missing"); return; }
    void *klass = p_class_from_name(image, "", name);
    if (!klass) { TWEAK_LOG("probe: class %s not found", name); return; }
    TWEAK_LOG("===== probe %s =====", name);
    void *iter = NULL;
    void *field;
    int count = 0;
    while ((field = p_class_get_fields(klass, &iter)) != NULL && count < 200) {
        const char *fname = p_field_get_name(field);
        int flags = p_field_get_flags ? p_field_get_flags(field) : 0;
        const char *ftype = "?";
        if (p_field_get_type && p_type_get_name) {
            void *t = p_field_get_type(field);
            if (t) ftype = p_type_get_name(t);
        }
        TWEAK_LOG("  %s%s : %s", (flags & 0x10) ? "static " : "", fname ? fname : "?", ftype ? ftype : "?");
        count++;
    }
    TWEAK_LOG("===== probe %s end (%d fields) =====", name, count);
}

// ---- 已解析特性(启动时解析一次,tick 时直接写) ----
typedef struct {
    int      active;
    void    *field;        // 目标字段
    int      is_static;
    int      type_kind;    // 0=int 1=float 2=bool 3=long
    long     target;
    // 实例链(可选): holder 静态字段 → GameObject → GetComponent(chain_type) → instance.field
    void    *holder_static_field;   // GameAPP.board
    void    *chain_method;          // GameObject.GetComponent(Type)
    void    *chain_type_obj;        // System.Type 实例(Board)
} ResolvedFeature;

static ResolvedFeature g_feats[UNITY_MAX_FEATURES];
static int g_feat_count = 0;
static int g_tick_ms = 300;

static void resolve_feature(void *image, UnityFeature *uf, ResolvedFeature *rf) {
    memset(rf, 0, sizeof(*rf));
    void *klass = p_class_from_name(image, uf->ns[0] ? uf->ns : "", uf->klass);
    if (!klass) { TWEAK_LOG("unity: class %s not found", uf->klass); return; }
    void *field = p_class_get_field_from_name(klass, uf->field);
    if (!field) { TWEAK_LOG("unity: field %s.%s not found", uf->klass, uf->field); return; }
    int flags = p_field_get_flags ? p_field_get_flags(field) : 0;
    rf->field = field;
    rf->is_static = (flags & 0x10) ? 1 : 0;
    rf->target = parse_value(uf->value_s);
    rf->type_kind = strcmp(uf->type, "float") == 0 ? 1 :
                    strcmp(uf->type, "bool") == 0 ? 2 :
                    strcmp(uf->type, "long") == 0 ? 3 : 0;
    rf->active = 1;

    // 实例字段:走 holder 静态字段链(uf->chain_holder="GameAPP.board", chain_class="Board")
    if (!rf->is_static && uf->chain_holder[0] && uf->chain_class[0]) {
        char hcls[64] = {0}, hfld[64] = {0};
        char *dot = strchr(uf->chain_holder, '.');
        if (!dot) { TWEAK_LOG("unity: chain_holder 需 Class.field 形式"); rf->active = 0; return; }
        size_t cl = (size_t)(dot - uf->chain_holder);
        if (cl >= sizeof(hcls)) cl = sizeof(hcls) - 1;
        memcpy(hcls, uf->chain_holder, cl);
        snprintf(hfld, sizeof(hfld), "%s", dot + 1);
        void *hk = p_class_from_name(image, "", hcls);
        if (!hk) { TWEAK_LOG("unity: chain holder class %s not found", hcls); rf->active = 0; return; }
        rf->holder_static_field = p_class_get_field_from_name(hk, hfld);
        if (!rf->holder_static_field) { TWEAK_LOG("unity: chain holder field %s not found", hfld); rf->active = 0; return; }
        void *ck = p_class_from_name(image, "", uf->chain_class);
        if (!ck) { TWEAK_LOG("unity: chain class %s not found", uf->chain_class); rf->active = 0; return; }
        // GameObject.GetComponent(Type)(非泛型重载,1 参)
        void *gok = p_class_from_name(image, "UnityEngine", "GameObject");
        if (!gok || !p_class_get_method_from_name || !p_runtime_invoke ||
            !p_type_get_object || !p_class_get_type) {
            TWEAK_LOG("unity: chain api incomplete"); rf->active = 0; return;
        }
        rf->chain_method = p_class_get_method_from_name(gok, "GetComponent", 1);
        if (!rf->chain_method) { TWEAK_LOG("unity: GameObject.GetComponent(Type) not found"); rf->active = 0; return; }
        rf->chain_type_obj = p_type_get_object(p_class_get_type(ck));
        if (!rf->chain_type_obj) { TWEAK_LOG("unity: chain type obj fail"); rf->active = 0; return; }
        TWEAK_LOG("unity: chain %s.%s via %s -> %s ready", uf->klass, uf->field, uf->chain_holder, uf->chain_class);
    }
    TWEAK_LOG("unity: feature %s.%s = %ld (%s%s)", uf->klass, uf->field, rf->target,
              rf->type_kind == 1 ? "float" : rf->type_kind == 2 ? "bool" : rf->type_kind == 3 ? "long" : "int",
              rf->is_static ? ", static" : ", instance");
}

// 单特性一次写
static void tick_feature(ResolvedFeature *rf) {
    if (!rf->active || !rf->field) return;
    void *obj = NULL;
    if (rf->is_static) {
        if (rf->type_kind == 1) { float v = (float)rf->target; p_field_static_set_value(rf->field, &v, &v); }
        else if (rf->type_kind == 2) { unsigned char v = rf->target ? 1 : 0; p_field_static_set_value(rf->field, &v, &v); }
        else if (rf->type_kind == 3) { long long v = (long long)rf->target; p_field_static_set_value(rf->field, &v, &v); }
        else { int v = (int)rf->target; p_field_static_set_value(rf->field, &v, &v); }
        return;
    }
    // 实例链:holder 静态字段(GameObject)→ GetComponent → 实例
    if (!rf->holder_static_field || !rf->chain_method || !rf->chain_type_obj) return;
    obj = NULL;
    p_field_static_get_value(rf->holder_static_field, &obj);
    if (!obj) return; // 场景未加载
    void *exc = NULL;
    void *args[1] = { rf->chain_type_obj };
    void *comp = p_runtime_invoke(rf->chain_method, obj, args, &exc);
    if (!comp || exc) return; // 无组件/异常
    if (rf->type_kind == 1) { float v = (float)rf->target; p_field_set_value(comp, rf->field, &v); }
    else if (rf->type_kind == 2) { unsigned char v = rf->target ? 1 : 0; p_field_set_value(comp, rf->field, &v); }
    else if (rf->type_kind == 3) { long long v = (long long)rf->target; p_field_set_value(comp, rf->field, &v); }
    else { int v = (int)rf->target; p_field_set_value(comp, rf->field, &v); }
}

void *unity_worker(void *arg) {
    TweakConfig *cfg = arg;
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

    if (cfg->probe_count > 0) {
        for (int i = 0; i < cfg->probe_count && i < 8; i++)
            probe_class(image, cfg->probe_classes[i]);
        TWEAK_LOG("probe done, exit");
        free(cfg);
        return NULL;
    }

    // 解析全部特性
    for (int fi = 0; fi < UNITY_MAX_FEATURES; fi++) {
        UnityFeature *uf = &cfg->unity[fi];
        if (!uf->klass[0] || !uf->field[0]) continue;
        if (g_feat_count >= UNITY_MAX_FEATURES) break;
        resolve_feature(image, uf, &g_feats[g_feat_count]);
        if (g_feats[g_feat_count].active) g_feat_count++;
    }
    if (g_feat_count == 0) { TWEAK_LOG("unity: no active features, worker exit"); free(cfg); return NULL; }
    TWEAK_LOG("unity: %d features active, tick=%dms", g_feat_count,
              cfg->poll_ms > 0 ? cfg->poll_ms : 300);

    for (;;) {
        for (int i = 0; i < g_feat_count; i++) tick_feature(&g_feats[i]);
        int pm = cfg->poll_ms > 0 ? cfg->poll_ms : 300;
        usleep(pm * 1000);
    }
}
