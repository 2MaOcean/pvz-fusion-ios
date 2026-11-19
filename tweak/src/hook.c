// hook.c — 内存读写 / 符号定位 / 补丁原语（arm64 iOS）
// 非越狱：仅写堆与 __DATA/__DATA_CONST；越狱或 TrollStore（amfi 降级）可写 __TEXT。
#include "tweak_main.h"
#include "pvz_tweak.h"
#include "mach_compat.h"
#include <stdarg.h>

// 日志：stderr + 追加写沙盒 Documents/pvztweak.log（手机文件 App 可直接查看）
void tweak_log(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    fprintf(stderr, "[PvZTweak] %s\n", buf);

    const char *home = getenv("HOME");
    if (!home || !home[0]) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/Documents/pvztweak.log", home);
    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "%s\n", buf);
        fclose(f);
    }
}

// ---------- 镜像定位 ----------
uintptr_t mem_image_base(const char *keyword) {
    if (!keyword || !keyword[0]) keyword = "PvZ";
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *name = _dyld_get_image_name(i);
        if (name && strstr(name, keyword))
            return (uintptr_t)_dyld_get_image_header(i);
    }
    return (uintptr_t)_dyld_get_image_header(0); // 兜底：主程序
}

// ---------- 符号定位 ----------
// 优先 dlsym（移植版若导出符号），否则扫描镜像 symtab。
void *mem_find_symbol(const char *image_keyword, const char *symbol) {
    if (!symbol || !symbol[0]) return NULL;

    void *p = dlsym(RTLD_DEFAULT, symbol);
    if (p) return p;

    uintptr_t base = mem_image_base(image_keyword);
    if (!base) return NULL;

    struct mach_header_64 *mh = (struct mach_header_64 *)base;
    struct segment_command_64 *le = NULL;
    struct symtab_command *symtab = NULL;

    uint8_t *cmd = (uint8_t *)mh + sizeof(*mh);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        struct load_command *lc = (struct load_command *)cmd;
        if (lc->cmd == LC_SEGMENT_64) {
            struct segment_command_64 *seg = (struct segment_command_64 *)cmd;
            if (strcmp(seg->segname, "__LINKEDIT") == 0) le = seg;
        } else if (lc->cmd == LC_SYMTAB) {
            symtab = (struct symtab_command *)cmd;
        }
        cmd += lc->cmdsize;
    }
    if (!le || !symtab) return NULL;

    unsigned long le_size = 0;
    uint8_t *le_mem = getsegmentdata(mh, "__LINKEDIT", &le_size);
    if (!le_mem) return NULL;

    // __TEXT 段 vmaddr 推 slide（主程序 0x100000000，dylib 0）
    intptr_t slide = 0;
    cmd = (uint8_t *)mh + sizeof(*mh);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        struct load_command *lc = (struct load_command *)cmd;
        if (lc->cmd == LC_SEGMENT_64) {
            struct segment_command_64 *seg = (struct segment_command_64 *)cmd;
            if (strcmp(seg->segname, "__TEXT") == 0) {
                slide = (intptr_t)mh - (intptr_t)seg->vmaddr;
                break;
            }
        }
        cmd += lc->cmdsize;
    }

    struct nlist_64 *nl = (struct nlist_64 *)(le_mem + (symtab->symoff - le->fileoff));
    char *strtab = (char *)(le_mem + (symtab->stroff - le->fileoff));
    for (uint32_t i = 0; i < symtab->nsyms; i++, nl++) {
        if (!nl->n_value || !nl->n_un.n_strx) continue;
        const char *n = strtab + nl->n_un.n_strx;
        if (strcmp(n, symbol) == 0)
            return (void *)(nl->n_value + slide);
    }
    return NULL;
}

// ---------- 页级写内存 ----------
// 先试 +VM_PROT_COPY（写时复制，任意页可用），失败再直接 +W。
// 写完尽力恢复为 R|X；数据页恢复失败无副作用。
static kern_return_t vm_write_range(uintptr_t addr, const void *data, size_t len, bool code) {
    mach_port_t self = mach_task_self();
    const vm_size_t page = 4096;
    uintptr_t cur = addr, end = addr + len;
    const uint8_t *src = data;

    while (cur < end) {
        uintptr_t page_start = cur & ~(page - 1);
        uintptr_t page_end = page_start + page;
        size_t chunk = (end < page_end ? end : page_end) - cur;

        kern_return_t kr = vm_protect(self, (vm_address_t)page_start, page, 0,
                                      VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
        if (kr != KERN_SUCCESS)
            kr = vm_protect(self, (vm_address_t)page_start, page, 0,
                            VM_PROT_READ | VM_PROT_WRITE);
        if (kr != KERN_SUCCESS) {
            TWEAK_LOG("vm_protect(+W) failed @0x%lx kr=%d (sandboxed __TEXT?)",
                      (unsigned long)page_start, kr);
            return kr;
        }
        memcpy((void *)cur, src, chunk);
        vm_protect(self, (vm_address_t)page_start, page, 0,
                   VM_PROT_READ | VM_PROT_EXECUTE);
        if (code) sys_icache_invalidate((void *)cur, chunk);

        cur += chunk;
        src += chunk;
    }
    return KERN_SUCCESS;
}

bool mem_patch(uintptr_t addr, const void *data, size_t len) {
    if (!addr) return false;
    return vm_write_range(addr, data, len, false) == KERN_SUCCESS;
}

bool patch_code_bytes(uintptr_t addr, const void *data, size_t len) {
    if (!addr) return false;
    return vm_write_range(addr, data, len, true) == KERN_SUCCESS;
}

bool mem_read(uintptr_t addr, void *out, size_t len) {
    if (!addr) return false;
    memcpy(out, (const void *)addr, len);
    return true;
}

uintptr_t mem_deref(uintptr_t addr) {
    uintptr_t v = 0;
    if (!addr) return 0;
    if (!mem_read(addr, &v, sizeof(v))) return 0;
    return v;
}
