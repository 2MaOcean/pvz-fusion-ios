// mach_compat.h — no-SDK 构建兼容头：内联声明 libc / Mach / Mach-O / dyld / CoreFoundation
// 供 zig cc 交叉编译 arm64 iOS dylib（-undefined dynamic_lookup 运行时由 dyld 绑定）。
// 全部为 Apple libSystem 公开 ABI。
#ifndef MACH_COMPAT_H
#define MACH_COMPAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>   // 编译器自带头，不依赖 libc

#ifdef USING_SDK
// CI/macOS SDK 构建：直接用 SDK 的系统头
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <pthread.h>
#include <mach/mach.h>
#include <libkern/OSCacheControl.h>   // sys_icache_invalidate
#else
// ---- libc 子集（libSystem 导出，no-SDK 手写声明） ----
typedef struct __sFILE FILE;
extern FILE *stderr;
extern int fprintf(FILE *, const char *, ...);
extern int vfprintf(FILE *, const char *, __builtin_va_list);
extern int vsnprintf(char *, unsigned long, const char *, __builtin_va_list);
extern int fclose(FILE *);
extern FILE *fopen(const char *, const char *);
extern char *getenv(const char *);

extern void *memcpy(void *, const void *, unsigned long);
extern void *memset(void *, int, unsigned long);
extern int strcmp(const char *, const char *);
extern char *strstr(const char *, const char *);
extern char *strncpy(char *, const char *, unsigned long);
extern unsigned long strlen(const char *);
extern void *malloc(unsigned long);
extern void free(void *);
extern int usleep(unsigned int);
extern long strtol(const char *, char **, int);
extern int snprintf(char *, unsigned long, const char *, ...);

typedef void *pthread_t;
extern int pthread_create(pthread_t *, const void *, void *(*)(void *), void *);
extern int pthread_detach(pthread_t);

extern void *dlsym(void *, const char *);
extern void *dlopen(const char *, int);
#define RTLD_DEFAULT ((void *)-2)
#define RTLD_LAZY   1
#define RTLD_GLOBAL 0x100

// ---- 基础 Mach 类型 ----
typedef int32_t   kern_return_t;
typedef int32_t   vm_prot_t;
typedef int32_t   boolean_t;
typedef uint32_t  mach_port_t;
typedef uint32_t  mach_msg_type_number_t;
typedef uint32_t  natural_t;
typedef unsigned long vm_address_t;
typedef unsigned long vm_size_t;

// ---- Mach 内核调用 ----
extern mach_port_t mach_task_self_(void);
#define mach_task_self() mach_task_self_()
extern kern_return_t vm_protect(mach_port_t target_task, vm_address_t address,
                                vm_size_t size, boolean_t set_maximum,
                                vm_prot_t new_protection);
extern void sys_icache_invalidate(void *start, size_t len);

#define MACH_PORT_NULL 0
#define KERN_SUCCESS        0
#define KERN_FAILURE        5
#define KERN_INVALID_TASK   6
#define KERN_INVALID_ARGUMENT 4
#define VM_PROT_READ    0x01
#define VM_PROT_WRITE   0x02
#define VM_PROT_EXECUTE 0x04
#define VM_PROT_COPY    0x10

#endif // USING_SDK

// ---- dyld API（两环境通用） ----
extern uint32_t _dyld_image_count(void);
extern const char *_dyld_get_image_name(uint32_t image_index);
extern const void *_dyld_get_image_header(uint32_t image_index);

// ---- Mach-O 常量与结构（公开格式） ----
#define MH_MAGIC_64   0xfeedfacf
#define LC_SYMTAB     0x02
#define LC_SEGMENT_64 0x19

struct mach_header_64 {
    uint32_t magic;
    int32_t  cputype;
    int32_t  cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
    uint32_t reserved;
};

struct load_command {
    uint32_t cmd;
    uint32_t cmdsize;
};

struct segment_command_64 {
    uint32_t cmd;
    uint32_t cmdsize;
    char     segname[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    int32_t  maxprot;
    int32_t  initprot;
    uint32_t nsects;
    uint32_t flags;
};

struct symtab_command {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t symoff;
    uint32_t nsyms;
    uint32_t stroff;
    uint32_t strsize;
};

struct nlist_64 {
    union { uint32_t n_strx; } n_un;
    uint8_t  n_type;
    uint8_t  n_sect;
    uint16_t n_desc;
    uint64_t n_value;
};

extern uint8_t *getsegmentdata(struct mach_header_64 *mh, const char *segname,
                               unsigned long *size);

// ---- CoreFoundation（不透明类型 + 所需 API 子集） ----
typedef int32_t SInt32;
typedef int64_t SInt64;
typedef long CFIndex;
typedef unsigned long CFTypeID;
typedef unsigned long CFOptionFlags;
typedef unsigned char Boolean;

typedef const struct __CFString     *CFStringRef;
typedef const struct __CFDictionary *CFDictionaryRef;
typedef const struct __CFData       *CFDataRef;
typedef const struct __CFURL        *CFURLRef;
typedef const void                  *CFPropertyListRef;
typedef const struct __CFNumber     *CFNumberRef;
typedef const struct __CFArray      *CFArrayRef;
typedef const struct __CFBoolean    *CFBooleanRef;
typedef struct __CFError            *CFErrorRef;
typedef struct __CFBundle           *CFBundleRef;
typedef const void                  *CFAllocatorRef;
typedef CFPropertyListRef            CFTypeRef;

#define kCFStringEncodingUTF8 0x08000100u
#define kCFNumberSInt64Type   4
#define kCFPropertyListMutableContainers 2

extern CFStringRef CFStringCreateWithCString(CFAllocatorRef alloc, const char *cStr,
                                             unsigned long encoding);
extern Boolean CFStringGetCString(CFStringRef theString, char *buffer, CFIndex bufferSize,
                                  unsigned long encoding);
extern CFTypeRef CFDictionaryGetValue(CFDictionaryRef theDict, const void *key);
extern Boolean CFDictionaryGetValueIfPresent(CFDictionaryRef theDict, const void *key,
                                             const void **value);
extern CFTypeID CFGetTypeID(CFTypeRef cf);
extern CFTypeID CFStringGetTypeID(void);
extern CFTypeID CFDictionaryGetTypeID(void);
extern CFTypeID CFBooleanGetTypeID(void);
extern CFTypeID CFNumberGetTypeID(void);
extern Boolean CFBooleanGetValue(CFBooleanRef boolean);
extern Boolean CFNumberGetValue(CFNumberRef number, long theType, void *valuePtr);
extern void CFRelease(CFTypeRef cf);
extern CFIndex CFArrayGetCount(CFArrayRef theArray);
extern const void *CFArrayGetValueAtIndex(CFArrayRef theArray, CFIndex idx);
extern CFTypeID CFArrayGetTypeID(void);
extern CFBundleRef CFBundleGetMainBundle(void);
extern CFURLRef CFBundleCopyResourcesDirectoryURL(CFBundleRef bundle);
extern CFURLRef CFURLCreateCopyAppendingPathComponent(CFAllocatorRef allocator,
                                                      CFURLRef url, CFStringRef pathComponent,
                                                      Boolean isDirectory);
extern Boolean CFURLCreateDataAndPropertiesFromResource(CFAllocatorRef alloc, CFURLRef url,
                                                        CFDataRef *data,
                                                        CFDictionaryRef *properties,
                                                        CFDictionaryRef desiredProperties,
                                                        SInt32 *errorCode);
extern CFPropertyListRef CFPropertyListCreateWithData(CFAllocatorRef allocator, CFDataRef data,
                                                      CFOptionFlags options,
                                                      CFStringRef *errorString,
                                                      CFErrorRef *error);

#endif
