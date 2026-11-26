// unity_tweak.c — Unity il2cpp 运行时反射修改器 v2(版本无关)
// 原理：dlopen UnityFramework → il2cpp_* C API → 运行时定位类/字段 → 周期性写值
// v2: 多特性并发 tick(修复单特性死循环)、long 类型、实例字段链锁定
//     (GameAPP.board → GameObject.GetComponent(typeof(Board)) → Board.theSun)
#include "tweak_main.h"
#include "pvz_tweak.h"
#include "tweak_ctl.h"

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

// ---- v0.5 runtime_invoke 设施 ----
static void *(*p_value_box)(void *, void *);   // il2cpp_value_box(klass, data)

// System 基本类型缓存
static void *k_int32, *k_int64, *k_boolean, *k_single, *k_string_klass;

static void init_system_types(void *image) {
    k_int32    = p_class_from_name(image, "System", "Int32");
    k_int64    = p_class_from_name(image, "System", "Int64");
    k_boolean  = p_class_from_name(image, "System", "Boolean");
    k_single   = p_class_from_name(image, "System", "Single");
    k_string_klass = p_class_from_name(image, "System", "String");
    p_value_box = dlsym(RTLD_DEFAULT, "il2cpp_value_box");
    TWEAK_LOG("invoke: sys types %s", (k_int32 && p_value_box) ? "ok" : "partial");
}

static void *box_i(int v)    { return p_value_box ? p_value_box(k_int32, &v) : NULL; }
static void *box_l(long long v) { return p_value_box ? p_value_box(k_int64, &v) : NULL; }
static void *box_b(unsigned char v) { return p_value_box ? p_value_box(k_boolean, &v) : NULL; }
static void *box_f(float v)  { return p_value_box ? p_value_box(k_single, &v) : NULL; }

static void *invoke_static_nm(void *image, const char *cls, const char *meth,
                              void **args, int argc) {
    void *k = p_class_from_name(image, "", cls);
    if (!k) { TWEAK_LOG("invoke: class %s not found", cls); return NULL; }
    void *m = p_class_get_method_from_name(k, meth, argc);
    if (!m) { TWEAK_LOG("invoke: %s.%s/%d not found", cls, meth, argc); return NULL; }
    void *exc = NULL;
    void *r = p_runtime_invoke(m, NULL, args, &exc);
    if (exc) TWEAK_LOG("invoke: %s.%s exception", cls, meth);
    return r;
}

static void *invoke_instance_nm(void *obj, void *klass, const char *meth,
                                void **args, int argc) {
    if (!obj || !klass) return NULL;
    void *m = p_class_get_method_from_name(klass, meth, argc);
    if (!m) return NULL;
    void *exc = NULL;
    return p_runtime_invoke(m, obj, args, &exc);
}

// ---- 已解析特性(启动时解析一次,tick 时直接写) ----
typedef struct {
    int      active;
    void    *field;
    int      is_static;
    int      type_kind;    // 0=int 1=float 2=bool 3=long
    long     target;
    char     fname[64];    // 字段名(菜单开关匹配)
    void    *holder_static_field;
    void    *chain_method;
    void    *chain_type_obj;
} ResolvedFeature;

static int save_t0_external = 0;  // 菜单触发立即落盘
static int t0_reset = 0;          // 菜单触发跳转计时重置
static ResolvedFeature g_feats[UNITY_MAX_FEATURES];
static int g_feat_count = 0;

static void resolve_feature(void *image, UnityFeature *uf, ResolvedFeature *rf) {
    memset(rf, 0, sizeof(*rf));
    void *klass = p_class_from_name(image, uf->ns[0] ? uf->ns : "", uf->klass);
    if (!klass) { TWEAK_LOG("unity: class %s not found", uf->klass); return; }
    void *field = p_class_get_field_from_name(klass, uf->field);
    if (!field) { TWEAK_LOG("unity: field %s.%s not found", uf->klass, uf->field); return; }
    int flags = p_field_get_flags ? p_field_get_flags(field) : 0;
    rf->field = field;
    snprintf(rf->fname, sizeof(rf->fname), "%s", uf->field);
    rf->is_static = (flags & 0x10) ? 1 : 0;
    rf->target = parse_value(uf->value_s);
    rf->type_kind = strcmp(uf->type, "float") == 0 ? 1 :
                    strcmp(uf->type, "bool") == 0 ? 2 :
                    strcmp(uf->type, "long") == 0 ? 3 : 0;
    rf->active = 1;

    if (!rf->is_static && uf->chain_holder[0] && uf->chain_class[0]) {
        char hcls[64] = {0}, hfld[64] = {0};
        char *dot = strchr(uf->chain_holder, '.');
        if (!dot) { rf->active = 0; return; }
        size_t cl = (size_t)(dot - uf->chain_holder);
        if (cl >= sizeof(hcls)) cl = sizeof(hcls) - 1;
        memcpy(hcls, uf->chain_holder, cl);
        snprintf(hfld, sizeof(hfld), "%s", dot + 1);
        void *hk = p_class_from_name(image, "", hcls);
        rf->holder_static_field = hk ? p_class_get_field_from_name(hk, hfld) : NULL;
        void *ck = p_class_from_name(image, "", uf->chain_class);
        void *gok = p_class_from_name(image, "UnityEngine", "GameObject");
        if (!rf->holder_static_field || !ck || !gok || !p_class_get_method_from_name ||
            !p_runtime_invoke || !p_type_get_object || !p_class_get_type) {
            TWEAK_LOG("unity: chain setup fail for %s.%s", uf->klass, uf->field);
            rf->active = 0; return;
        }
        rf->chain_method = p_class_get_method_from_name(gok, "GetComponent", 1);
        rf->chain_type_obj = p_type_get_object(p_class_get_type(ck));
        if (!rf->chain_method || !rf->chain_type_obj) { rf->active = 0; return; }
        TWEAK_LOG("unity: chain %s.%s via %s -> %s ready", uf->klass, uf->field, uf->chain_holder, uf->chain_class);
    }
    TWEAK_LOG("unity: feature %s.%s = %ld (%s%s)", uf->klass, uf->field, rf->target,
              rf->type_kind == 1 ? "float" : rf->type_kind == 2 ? "bool" : rf->type_kind == 3 ? "long" : "int",
              rf->is_static ? ", static" : ", instance");
}

static void tick_feature(ResolvedFeature *rf) {
    if (!rf->active || !rf->field) return;
    if (rf->is_static) {
        if (rf->type_kind == 1) { float v = (float)rf->target; p_field_static_set_value(rf->field, &v, &v); }
        else if (rf->type_kind == 2) { unsigned char v = rf->target ? 1 : 0; p_field_static_set_value(rf->field, &v, &v); }
        else if (rf->type_kind == 3) { long long v = (long long)rf->target; p_field_static_set_value(rf->field, &v, &v); }
        else { int v = (int)rf->target; p_field_static_set_value(rf->field, &v, &v); }
        return;
    }
    void *obj = NULL;
    p_field_static_get_value(rf->holder_static_field, &obj);
    if (!obj) return;
    void *exc = NULL;
    void *args[1] = { rf->chain_type_obj };
    void *comp = p_runtime_invoke(rf->chain_method, obj, args, &exc);
    if (!comp || exc) return;
    if (rf->type_kind == 1) { float v = (float)rf->target; p_field_set_value(comp, rf->field, &v); }
    else if (rf->type_kind == 2) { unsigned char v = rf->target ? 1 : 0; p_field_set_value(comp, rf->field, &v); }
    else if (rf->type_kind == 3) { long long v = (long long)rf->target; p_field_set_value(comp, rf->field, &v); }
    else { int v = (int)rf->target; p_field_set_value(comp, rf->field, &v); }
}

// ---- v0.5 高阶动作:模式跳转 / 全卡池 / 任意放置 / 存档落盘 ----
typedef struct {
    int  active;
    char mode[32];      // "IZ" / "adv" / "menu" / "treasure" / "tower" / "challenge"
    int  level;
    int  delay_ms;      // 主菜单就绪后延迟
    int  done;
} ModeJump;

static ModeJump g_jump;
static int g_seed_all = 0;      // Board.seedPool 塞满全植物
static int g_place_on_mouse = 0;// 周期把指定植物放上鼠标
static int g_place_planttype = 0;
static int g_save_flush_ms = 0; // 定时 SavePlayerData

// Board 实例(从 GameAPP.board 反射拿)
static void *get_board_component(void *image, void *board_go, const char *comp_class) {
    void *ck = p_class_from_name(image, "", comp_class);
    void *gok = p_class_from_name(image, "UnityEngine", "GameObject");
    if (!ck || !gok || !board_go) return NULL;
    void *m = p_class_get_method_from_name(gok, "GetComponent", 1);
    if (!m) return NULL;
    void *tobj = p_type_get_object(p_class_get_type(ck));
    if (!tobj) return NULL;
    void *exc = NULL;
    void *args[1] = { tobj };
    return p_runtime_invoke(m, board_go, args, &exc);
}

static void do_seed_all(void *image, void *board_go) {
    // Board.seedPool (List<PlantType>) 塞入 PlantType 枚举全值
    void *board = get_board_component(image, board_go, "Board");
    if (!board) return;
    void *bk = p_class_from_name(image, "", "Board");
    void *fld = p_class_get_field_from_name(bk, "seedPool");
    if (!fld) { TWEAK_LOG("seed_all: Board.seedPool not found"); return; }
    void *list = NULL;
    p_field_get_value(board, fld, &list);
    if (!list) { TWEAK_LOG("seed_all: seedPool null"); return; }
    void *list_k = p_class_from_name(image, "System.Collections.Generic", "List`1");
    void *add = list_k ? p_class_get_method_from_name(list_k, "Add", 1) : NULL;
    // 泛型 List 的 Add 通过实例方法可能查不到(开放泛型),退回 runtime_invoke 按 obj 的实际类
    if (!add) {
        void *obj_k = p_class_from_name(image, "", "Board");
        (void)obj_k;
        // 用对象自身类的方法表:il2cpp_object_get_class
        void *(*obj_get_class)(void *) = dlsym(RTLD_DEFAULT, "il2cpp_object_get_class");
        if (obj_get_class) {
            void *real_k = obj_get_class(list);
            add = p_class_get_method_from_name(real_k, "Add", 1);
        }
    }
    if (!add) { TWEAK_LOG("seed_all: List.Add not found"); return; }
    void *pt_k = p_class_from_name(image, "", "PlantType");
    if (!pt_k) { TWEAK_LOG("seed_all: PlantType not found"); return; }
    void *m_getnames = p_class_get_method_from_name(pt_k, "GetNames", 0);
    (void)m_getnames;
    // 枚举范围:PlantsVsZombiesRH PlantType 值域 0..~300;直接 0..299 塞入
    int added = 0;
    for (int v = 0; v < 300; v++) {
        void *boxed = p_value_box(pt_k, &v);
        if (!boxed) break;
        void *exc = NULL;
        void *args[1] = { boxed };
        p_runtime_invoke(add, list, args, &exc);
        if (!exc) added++;
    }
    TWEAK_LOG("seed_all: added %d plants to seedPool", added);
    g_seed_all = 0; // 一次即可
}

static void do_place_on_mouse(void *image, void *board_go) {
    void *mouse = get_board_component(image, board_go, "Mouse");
    if (!mouse) return;
    void *mk = p_class_from_name(image, "", "Mouse");
    void *m = p_class_get_method_from_name(mk, "CreatePlantOnMouse", 1);
    if (!m) { TWEAK_LOG("place: Mouse.CreatePlantOnMouse not found"); g_place_on_mouse = 0; return; }
    void *pt_k = p_class_from_name(image, "", "PlantType");
    void *boxed = p_value_box ? p_value_box(pt_k, &g_place_planttype) : NULL;
    if (!boxed) return;
    void *exc = NULL;
    void *args[1] = { boxed };
    p_runtime_invoke(m, mouse, args, &exc);
    if (exc) { TWEAK_LOG("place: exception"); }
}

static void do_save_flush(void *image) {
    // SaveInfo.Instance(静态字段)→ SavePlayerData()
    void *si_k = p_class_from_name(image, "", "SaveInfo");
    if (!si_k) return;
    void *inst_f = p_class_get_field_from_name(si_k, "Instance");
    if (!inst_f) return;
    void *inst = NULL;
    p_field_static_get_value(inst_f, &inst);
    if (!inst) return;
    void *m = p_class_get_method_from_name(si_k, "SavePlayerData", 0);
    if (!m) { TWEAK_LOG("save: SavePlayerData not found"); return; }
    void *exc = NULL;
    p_runtime_invoke(m, inst, NULL, &exc);
    TWEAK_LOG("save: SavePlayerData invoked");
}

static void do_mode_jump(void *image) {
    void *uimgr_k = p_class_from_name(image, "", "UIMgr");
    if (!uimgr_k) { TWEAK_LOG("jump: UIMgr not found"); return; }
    void *exc = NULL;
    if (strcmp(g_jump.mode, "IZ") == 0) {
        void *m = p_class_get_method_from_name(uimgr_k, "EnterIZGame", 3);
        if (m) {
            void *a[3] = { box_i(g_jump.level), box_i(0), box_i(0) };
            // name 参数是 string:传 NULL(默认 null)——占位用 box_i 不对;直接 3 参:string=null
            void *args3[3] = { box_i(g_jump.level), NULL, NULL };
            p_runtime_invoke(m, NULL, args3, &exc);
        }
    } else if (strcmp(g_jump.mode, "adv") == 0) {
        void *m = p_class_get_method_from_name(uimgr_k, "EnterGame", 4);
        if (m) {
            void *lt_k = p_class_from_name(image, "", "LevelType");
            int lt = 0; // Advanture
            void *args4[4] = { p_value_box(lt_k, &lt), box_i(g_jump.level), box_i(0), NULL };
            p_runtime_invoke(m, NULL, args4, &exc);
        }
    } else if (strcmp(g_jump.mode, "menu") == 0) {
        void *m = p_class_get_method_from_name(uimgr_k, "EnterMainMenu", 0);
        if (m) p_runtime_invoke(m, NULL, NULL, &exc);
    } else if (strcmp(g_jump.mode, "treasure") == 0) {
        void *m = p_class_get_method_from_name(uimgr_k, "EnterTreasureMenu", 0);
        if (m) p_runtime_invoke(m, NULL, NULL, &exc);
    } else if (strcmp(g_jump.mode, "tower") == 0) {
        void *m = p_class_get_method_from_name(uimgr_k, "EnterTowerMenu", 0);
        if (m) p_runtime_invoke(m, NULL, NULL, &exc);
    } else if (strcmp(g_jump.mode, "challenge") == 0) {
        void *m = p_class_get_method_from_name(uimgr_k, "EnterChallengeMenu", 0);
        if (m) p_runtime_invoke(m, NULL, NULL, &exc);
    } else if (strcmp(g_jump.mode, "izmenu") == 0) {
        void *m = p_class_get_method_from_name(uimgr_k, "EnterIZMenu", 0);
        if (m) p_runtime_invoke(m, NULL, NULL, &exc);
    }
    TWEAK_LOG("jump: %s(%d) %s", g_jump.mode, g_jump.level, exc ? "exception" : "invoked");
    g_jump.done = 1;
}

void *unity_worker(void *arg) {
    TweakConfig *cfg = arg;
    for (int i = 0; i < 1800 && !api_ready(); i++) {
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

    init_system_types(image);

    // 解析全部锁定特性
    for (int fi = 0; fi < UNITY_MAX_FEATURES; fi++) {
        UnityFeature *uf = &cfg->unity[fi];
        if (!uf->klass[0] || !uf->field[0]) continue;
        if (g_feat_count >= UNITY_MAX_FEATURES) break;
        resolve_feature(image, uf, &g_feats[g_feat_count]);
        if (g_feats[g_feat_count].active) g_feat_count++;
    }
    TWEAK_LOG("unity: %d features active", g_feat_count);

    // v0.5 动作配置
    g_seed_all = cfg->seed_pool_all;
    g_place_on_mouse = cfg->place_on_mouse;
    g_place_planttype = cfg->place_plant_type;
    g_save_flush_ms = cfg->save_flush_ms;
    if (cfg->jump_mode[0]) {
        snprintf(g_jump.mode, sizeof(g_jump.mode), "%s", cfg->jump_mode);
        g_jump.level = cfg->jump_level;
        g_jump.delay_ms = cfg->jump_delay_ms > 0 ? cfg->jump_delay_ms : 30000;
        g_jump.done = 0;
    }

    long long t0 = 0;
    long long place_t0 = 0;
    long long save_t0 = 0;
    for (;;) {
        for (int i = 0; i < g_feat_count; i++) tick_feature(&g_feats[i]);

        void *domain = p_domain_get();
        void *board_go = NULL;
        if (domain) {
            void *gapp = p_class_from_name(image, "", "GameAPP");
            void *bf = gapp ? p_class_get_field_from_name(gapp, "board") : NULL;
            if (bf) p_field_static_get_value(bf, &board_go);
        }

        // 菜单触发:立即落盘
        if (save_t0_external) { do_save_flush(image); save_t0_external = 0; }
        // 模式跳转(延迟执行)
        if (t0_reset) { t0 = 0; t0_reset = 0; }
        if (g_jump.mode[0] && !g_jump.done) {
            long long now = t0 += (cfg->poll_ms > 0 ? cfg->poll_ms : 300);
            (void)now;
            if (t0 >= g_jump.delay_ms) { do_mode_jump(image); }
        }
        // 全卡池(board 就绪后)
        if (g_seed_all && board_go) { do_seed_all(image, board_go); }
        // 任意放置(周期把植物放上鼠标)
        if (g_place_on_mouse && board_go) {
            place_t0 += (cfg->poll_ms > 0 ? cfg->poll_ms : 300);
            if (place_t0 >= 2000) { do_place_on_mouse(image, board_go); place_t0 = 0; }
        }
        // 存档落盘
        if (g_save_flush_ms > 0) {
            save_t0 += (cfg->poll_ms > 0 ? cfg->poll_ms : 300);
            if (save_t0 >= g_save_flush_ms) { do_save_flush(image); save_t0 = 0; }
        }

        int pm = cfg->poll_ms > 0 ? cfg->poll_ms : 300;
        usleep(pm * 1000);
    }
}

// ---- 菜单控制接口(menu.m 调用) ----
static int find_feat_by_field(const char *fname) {
    for (int i = 0; i < g_feat_count; i++)
        if (g_feats[i].active && g_feats[i].field) {
            // 用字段名匹配(启动时记录)
            if (strstr(g_feats[i].fname, fname)) return i;
        }
    return -1;
}

int ctl_sun_enabled(void) {
    int i = find_feat_by_field("theSun");
    return i >= 0 && g_feats[i].active;
}
int ctl_money_enabled(void) {
    int i = find_feat_by_field("theMoneyCount");
    return i >= 0 && g_feats[i].active;
}
int ctl_seed_all_pending(void) { return g_seed_all; }
int ctl_place_enabled(void) { return g_place_on_mouse; }

void ctl_sun_toggle(void) {
    int i = find_feat_by_field("theSun");
    if (i >= 0) { g_feats[i].active = !g_feats[i].active; TWEAK_LOG("menu: sun lock %s", g_feats[i].active ? "ON" : "OFF"); }
}
void ctl_money_toggle(void) {
    int i = find_feat_by_field("theMoneyCount");
    if (i >= 0) { g_feats[i].active = !g_feats[i].active; TWEAK_LOG("menu: money lock %s", g_feats[i].active ? "ON" : "OFF"); }
}
void ctl_seed_all_fire(void) {
    g_seed_all = 1;
    TWEAK_LOG("menu: seed_all fired");
}
void ctl_place_toggle(int plant) {
    if (plant >= 0) g_place_planttype = plant;
    g_place_on_mouse = !g_place_on_mouse;
    TWEAK_LOG("menu: place_on_mouse %s (type=%d)", g_place_on_mouse ? "ON" : "OFF", g_place_planttype);
}
void ctl_save_now(void) {
    g_save_flush_ms = g_save_flush_ms > 0 ? g_save_flush_ms : 60000;
    save_t0_external = 1; // 触发立即落盘
    TWEAK_LOG("menu: save now");
}
void ctl_jump(const char *mode, int level) {
    snprintf(g_jump.mode, sizeof(g_jump.mode), "%s", mode);
    g_jump.level = level;
    g_jump.done = 0;
    g_jump.delay_ms = 500; // 菜单触发时立即(500ms 内)
    t0_reset = 1;
    TWEAK_LOG("menu: jump %s level=%d", mode, level);
}
