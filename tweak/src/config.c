// config.c — 读取 App bundle 内 Offsets.plist（纯 CoreFoundation，无 ObjC）
#include "pvz_tweak.h"
#include "mach_compat.h"   // CF / libc no-SDK 声明

// bundle 内路径：用 CFBundleCopyResourcesDirectoryURL（C API）
static CFURLRef bundle_resource_dir(void) {
    void *(*bundle_main)(void) = dlsym(RTLD_DEFAULT, "CFBundleGetMainBundle");
    CFURLRef (*copy_res_dir)(void *) = dlsym(RTLD_DEFAULT, "CFBundleCopyResourcesDirectoryURL");
    if (!bundle_main || !copy_res_dir) return NULL;
    void *b = bundle_main();
    if (!b) return NULL;
    return copy_res_dir(b);
}

static CFDictionaryRef dict_of(CFDictionaryRef parent, const char *key) {
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFDictionaryRef d = CFDictionaryGetValue(parent, k);
    CFRelease(k);
    return d;
}

static void fill_feature(CFDictionaryRef parent, const char *key,
                         TweakFeature *f, const TweakFeature *def) {
    *f = def ? *def : (TweakFeature){0};
    if (!parent) return;
    CFDictionaryRef d = dict_of(parent, key);
    if (!d) return;
    CFStringRef ks;
    CFBooleanRef b;
    SInt64 n;

    ks = CFStringCreateWithCString(NULL, "enabled", kCFStringEncodingUTF8);
    const void *vp = NULL;
    if (CFDictionaryGetValueIfPresent(d, ks, &vp) && vp &&
        CFGetTypeID((CFTypeRef)vp) == CFBooleanGetTypeID())
        f->enabled = CFBooleanGetValue((CFBooleanRef)vp);
    CFRelease(ks);

    ks = CFStringCreateWithCString(NULL, "value", kCFStringEncodingUTF8);
    vp = NULL;
    if (CFDictionaryGetValueIfPresent(d, ks, &vp) && vp &&
        CFGetTypeID((CFTypeRef)vp) == CFNumberGetTypeID())
        CFNumberGetValue((CFNumberRef)vp, kCFNumberSInt64Type, &f->value);
    CFRelease(ks);

    struct { const char *k; char *dst; size_t sz; } strs[] = {
        {"symbol", f->symbol, sizeof(f->symbol)},
        {"patch_hex", f->patch_hex, sizeof(f->patch_hex)},
    };
    for (unsigned i = 0; i < 2; i++) {
        ks = CFStringCreateWithCString(NULL, strs[i].k, kCFStringEncodingUTF8);
        vp = NULL;
        if (CFDictionaryGetValueIfPresent(d, ks, &vp) && vp &&
            CFGetTypeID((CFTypeRef)vp) == CFStringGetTypeID())
            CFStringGetCString((CFStringRef)vp, strs[i].dst, (CFIndex)strs[i].sz, kCFStringEncodingUTF8);
        CFRelease(ks);
    }

    struct { const char *k; long *dst; } nums[] = {
        {"offset", &f->offset}, {"field_offset", &f->field_offset},
    };
    for (unsigned i = 0; i < 2; i++) {
        ks = CFStringCreateWithCString(NULL, nums[i].k, kCFStringEncodingUTF8);
        vp = NULL;
        if (CFDictionaryGetValueIfPresent(d, ks, &vp) && vp &&
            CFGetTypeID((CFTypeRef)vp) == CFNumberGetTypeID()) {
            SInt64 v = 0;
            CFNumberGetValue((CFNumberRef)vp, kCFNumberSInt64Type, &v);
            *nums[i].dst = (long)v;
        }
        CFRelease(ks);
    }
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool config_parse_patch_hex(const char *hex, uint8_t *out, size_t max, size_t *out_len) {
    size_t n = 0;
    for (const char *p = hex; p[0] && p[1]; p += 2) {
        while (*p == ' ') p++;
        if (!p[0] || !p[1]) break;
        int hi = hexval(p[0]), lo = hexval(p[1]);
        if (hi < 0 || lo < 0 || n >= max) return false;
        out[n++] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = n;
    return n > 0;
}

bool config_load(TweakConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->image_keyword, "PvZ", sizeof(cfg->image_keyword) - 1);
    cfg->poll_ms = 100;
    cfg->sun     = (TweakFeature){ .enabled = true, .value = 9990 };
    cfg->coins   = (TweakFeature){ .enabled = false };
    cfg->cooldown= (TweakFeature){ .enabled = false };
    cfg->no_drop = (TweakFeature){ .enabled = false };

    CFURLRef dir = bundle_resource_dir();
    if (!dir) { TWEAK_LOG("no bundle resource dir, use defaults"); return true; }
    CFStringRef name = CFStringCreateWithCString(NULL, "Offsets.plist", kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateCopyAppendingPathComponent(NULL, dir, name, false);
    CFRelease(dir); CFRelease(name);
    if (!url) return true;

    CFDataRef data = NULL;
    SInt32 err = 0;
    CFURLCreateDataAndPropertiesFromResource(NULL, url, &data, NULL, NULL, &err);
    CFRelease(url);
    if (!data) return true;

    CFErrorRef e = NULL;
    CFPropertyListRef pl = CFPropertyListCreateWithData(NULL, data, kCFPropertyListMutableContainers, NULL, &e);
    CFRelease(data);
    if (!pl || CFGetTypeID(pl) != CFDictionaryGetTypeID()) { if (e) CFRelease(e); return true; }

    CFDictionaryRef root = (CFDictionaryRef)pl;
    CFStringRef ks = CFStringCreateWithCString(NULL, "image_keyword", kCFStringEncodingUTF8);
    CFStringRef img;
    if (CFDictionaryGetValueIfPresent(root, ks, (const void **)&img) && CFGetTypeID(img) == CFStringGetTypeID())
        CFStringGetCString(img, cfg->image_keyword, sizeof(cfg->image_keyword), kCFStringEncodingUTF8);
    CFRelease(ks);

    fill_feature(root, "sun",      &cfg->sun,      &cfg->sun);
    fill_feature(root, "coins",    &cfg->coins,    &cfg->coins);
    fill_feature(root, "cooldown", &cfg->cooldown, &cfg->cooldown);
    fill_feature(root, "no_drop",  &cfg->no_drop,  &cfg->no_drop);

    ks = CFStringCreateWithCString(NULL, "poll_ms", kCFStringEncodingUTF8);
    CFNumberRef n;
    if (CFDictionaryGetValueIfPresent(root, ks, (const void **)&n) && CFGetTypeID(n) == CFNumberGetTypeID()) {
        SInt64 v; CFNumberGetValue(n, kCFNumberSInt64Type, &v); cfg->poll_ms = (int)v;
    }
    CFRelease(ks);

    // unity: [{class, ns?, field, type?, value, poll_ms?}, ...]
    {
        const void *vp = NULL;
        ks = CFStringCreateWithCString(NULL, "unity", kCFStringEncodingUTF8);
        if (CFDictionaryGetValueIfPresent(root, ks, &vp) && vp &&
            CFGetTypeID(vp) == CFArrayGetTypeID()) {
            CFArrayRef arr = (CFArrayRef)vp;
            CFTypeID arrT = CFArrayGetTypeID();
            (void)arrT;
            CFIndex n_items = CFArrayGetCount(arr);
            for (CFIndex i = 0; i < n_items && cfg->unity_count < 8; i++) {
                CFDictionaryRef item = (CFDictionaryRef)CFArrayGetValueAtIndex(arr, i);
                if (!item || CFGetTypeID(item) != CFDictionaryGetTypeID()) continue;
                struct { const char *key; char *dst; size_t sz; } strs[] = {
                    {"class", cfg->unity[i].klass, 64},
                    {"ns", cfg->unity[i].ns, 64},
                    {"field", cfg->unity[i].field, 64},
                    {"type", cfg->unity[i].type, 16},
                };
                for (unsigned u = 0; u < 4; u++) {
                    CFStringRef k2 = CFStringCreateWithCString(NULL, strs[u].key, kCFStringEncodingUTF8);
                    const void *v2 = NULL;
                    if (CFDictionaryGetValueIfPresent(item, k2, &v2) && v2 &&
                        CFGetTypeID(v2) == CFStringGetTypeID())
                        CFStringGetCString((CFStringRef)v2, strs[u].dst, (CFIndex)strs[u].sz, kCFStringEncodingUTF8);
                    CFRelease(k2);
                }
                // value: number 或 string
                long val = 0;
                CFStringRef k2 = CFStringCreateWithCString(NULL, "value", kCFStringEncodingUTF8);
                const void *v2 = NULL;
                if (CFDictionaryGetValueIfPresent(item, k2, &v2) && v2) {
                    if (CFGetTypeID(v2) == CFNumberGetTypeID()) {
                        SInt64 nn; CFNumberGetValue((CFNumberRef)v2, kCFNumberSInt64Type, &nn); val = (long)nn;
                    } else if (CFGetTypeID(v2) == CFStringGetTypeID()) {
                        char buf[32]; CFStringGetCString((CFStringRef)v2, buf, 32, kCFStringEncodingUTF8);
                        val = strtol(buf, NULL, 0);
                    }
                }
                CFRelease(k2);
                snprintf(cfg->unity[i].value_s, sizeof(cfg->unity[i].value_s), "%ld", val);
                cfg->unity[i].poll_ms = 300;
                cfg->unity_count++;
            }
        }
        CFRelease(ks);
    }

    CFRelease(pl);
    return true;
}
