// hook.c — 内存读写 / 符号定位 / 补丁原语（arm64 iOS）
// 非越狱：仅写堆与 __DATA/__DATA_CONST；越狱或 TrollStore（amfi 降级）可写 __TEXT。
#include "pvz_tweak.h"

#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

void tweak_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
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
// 返回运行时地址（含 ASLR slide）。
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

// ---------- 页级写内存（保留原页属性） ----------
static kern_return_t vm_write_range(uintptr_t addr, const void *data, size_t len, bool code) {
    mach_port_t self = mach_task_self();
    const vm_size_t page = 4096;
    uintptr_t cur = addr, end = addr + len;
    const uint8_t *src = data;

    while (cur < end) {
        uintptr_t page_start = cur & ~(page - 1);
        uintptr_t page_end = page_start + page;
        size_t chunk = (end < page_end ? end : page_end) - cur;

        // 查询本页所在 region 的当前/最大保护属性
        vm_address_t rbase = (vm_address_t)page_start;
        vm_size_t rsize = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        vm_prot_t saved = VM_PROT_READ;
        if (mach_vm_region(self, &rbase, &rsize, VM_REGION_BASIC_INFO_64,
                           (vm_region_info_t)&info, &cnt, &obj) == KERN_SUCCESS) {
            saved = info.protection;
            if (obj != MACH_PORT_NULL) mach_port_deallocate(self, obj);
        }

        kern_return_t kr = vm_protect(self, (vm_address_t)page_start, page, 0,
                                      saved | VM_PROT_WRITE);
        if (kr != KERN_SUCCESS) {
            TWEAK_LOG("vm_protect(+W) failed @0x%lx kr=%d (sandboxed __TEXT?)",
                      (unsigned long)page_start, kr);
            return kr;
        }
        memcpy((void *)cur, src, chunk);
        vm_protect(self, (vm_address_t)page_start, page, 0, saved);
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
