#!/usr/bin/env python3
# analyze_base.py — iOS 基座 ipa 一键分析（引擎判定 / 符号 / 资源结构）
# 用法: python3 tools/analyze_base.py <base.ipa> [报告输出路径]
import plistlib, struct, sys, zipfile, tempfile, os, shutil, re

MACH_MAGICS = {0xFEEDFACF: "64", 0xFEEDFACE: "32", 0xCFFAEDFE: "32LE", 0xCFAEDFE: "64LE"}

def u32(b, o): return struct.unpack_from("<I", b, o)[0]

def cstr(b, o):
    e = b.find(b"\0", o)
    return b[o:e].decode("utf-8", "ignore")

def parse_macho(b):
    """返回 (摘要 dict, 依赖列表, 导出符号采样)"""
    info = {"arch": None, "filetype": None, "flags": None, "min_ios": None,
            "sdk": None, "uuid": None, "has_code_signature": False,
            "ncmds": 0, "pie": False}
    deps, syms = [], []
    magic = u32(b, 0)
    if magic not in MACH_MAGICS:
        raise ValueError("not a mach-o")
    is64 = magic in (0xFEEDFACF, 0xCFAEDFE)
    ncmds, off = u32(b, 16), (32 if is64 else 28)
    info["ncmds"] = ncmds
    symtab_cmd = None
    linkedit = None
    text_seg = None
    for _ in range(ncmds):
        cmd, size = u32(b, off), u32(b, off + 4)
        if cmd == 0x19:  # LC_SEGMENT_64
            name = cstr(b, off + 8).rstrip("\0")
            if name == "__LINKEDIT":
                linkedit = struct.unpack_from("<QQQ", b, off + 32)  # vmaddr fileoff filesize
            if name == "__TEXT":
                text_seg = struct.unpack_from("<QQQ", b, off + 32)
        elif cmd in (0xC, 0xD, 0x80000022):  # LOAD_DYLIB / LOAD_WEAK_DYLIB / REEXPORT
            name_ofs = u32(b, off + 8)
            deps.append(("weak:" if cmd == 0xD else "") + cstr(b, off + name_ofs))
        elif cmd == 0x1B:  # LC_UUID
            info["uuid"] = b[off + 8:off + 24].hex()
        elif cmd == 0x2:  # LC_SYMTAB
            symtab_cmd = struct.unpack_from("<IIIII", b, off + 8)
        elif cmd == 0x32:  # LC_BUILD_VERSION
            info["sdk"] = f"{u32(b,off+12)>>16}.{(u32(b,off+12)>>8)&0xFF}"
            info["min_ios"] = f"{u32(b,off+8)>>16}.{(u32(b,off+8)>>8)&0xFF}"
        elif cmd == 0x1D:  # LC_CODE_SIGNATURE
            info["has_code_signature"] = True
        off += size
    if text_seg:
        slide = None
        info["pie"] = bool(u32(b, 24) & 0x200000)  # MH_PIE
    # 符号表采样
    if symtab_cmd and linkedit:
        _, _, symoff, nsyms, stroff, _ = symtab_cmd
        le_off_in_cmd = None
        # symtab off 是文件偏移；直接用
        try:
            strtab = b[symtab_cmd[0] + 0:0]  # placeholder
        except Exception:
            pass
        for i in range(min(nsyms, 4000)):
            so = symoff + i * 16
            if so + 16 > len(b):
                break
            n_strx, n_type, n_sect, n_desc, n_value = struct.unpack_from("<IBBHQ", b, so)
            if n_value and n_strx and (n_type & 0x0E) == 0x0E:  # N_EXT|N_SECT 定义符号
                name = cstr(b, symtab_cmd[4] + n_strx)
                if name and not name.startswith("__OBJC_$"):
                    syms.append(name)
                    if len(syms) >= 60:
                        break
    return info, deps, syms

ENGINE_MARKERS = {
    b"LawnMower": "PopCap Lawn 原版引擎移植",
    b"SexyAppBase": "PopCap SexyApp 框架",
    b"Lawn": "Lawn 引擎特征",
    b"SDL_": "SDL 移植",
    b"cocos2d": "cocos2d 移植",
    b"GameMaker": "GameMaker",
    b"Unity": "Unity",
    b"Reanimator": "原版 Reanimator 系统",
    b"main.pak": "原版 pak 资源系统",
    b"Fusion": "融合版字样",
    b"properties_lawn": "原版 properties 资源",
}

def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    ipa = sys.argv[1]
    work = tempfile.mkdtemp(prefix="base_")
    try:
        with zipfile.ZipFile(ipa) as z:
            z.extractall(work)
        app = None
        for root, dirs, _ in os.walk(os.path.join(work, "Payload")):
            for d in dirs:
                if d.endswith(".app"):
                    app = os.path.join(root, d); break
        if not app:
            raise SystemExit("未找到 .app")
        print(f"=== 基座: {os.path.basename(app)} ===")
        # Info.plist
        try:
            with open(os.path.join(app, "Info.plist"), "rb") as f:
                pl = plistlib.load(f)
            for k in ("CFBundleExecutable", "CFBundleIdentifier", "CFBundleShortVersionString",
                      "MinimumOSVersion", "CFBundleDisplayName"):
                print(f"  {k}: {pl.get(k)}")
            exe = pl.get("CFBundleExecutable", os.path.basename(app)[:-4])
        except Exception as e:
            print("  Info.plist 解析失败:", e); exe = os.path.basename(app)[:-4]
        # 主二进制
        binp = os.path.join(app, exe)
        with open(binp, "rb") as f:
            b = f.read()
        try:
            info, deps, syms = parse_macho(b)
            print(f"  二进制: {len(b)//1024} KB | ncmds={info['ncmds']} | PIE={info['pie']} "
                  f"| min_ios={info['min_ios']} | sdk={info['sdk']} | 签名={info['has_code_signature']}")
            print(f"  UUID: {info['uuid']}")
            print("  依赖库:")
            for d in deps:
                print(f"    - {d}")
            print(f"  导出符号采样({len(syms)}):")
            for s in syms[:40]:
                print(f"    {s}")
        except ValueError:
            print("  主二进制非 Mach-O（可能是加密壳或 fat）")
        # 引擎特征
        print("  引擎特征串:")
        for marker, label in ENGINE_MARKERS.items():
            cnt = b.count(marker)
            if cnt:
                print(f"    {marker.decode()}: {cnt} 次 -> {label}")
        # 资源结构
        exts, paks = {}, []
        total = 0
        for root, _, files in os.walk(app):
            for fn in files:
                p = os.path.join(root, fn)
                total += os.path.getsize(p)
                ext = os.path.splitext(fn)[1].lower()
                exts[ext] = exts.get(ext, 0) + 1
                if fn.lower().endswith((".pak", ".properties", ".xml", ".json")):
                    paks.append(os.path.relpath(p, app))
        print(f"  资源总大小: {total//1024//1024} MB")
        print("  扩展名分布:", dict(sorted(exts.items(), key=lambda x: -x[1])[:12]))
        print("  数据/配置文件:", paks[:20] if paks else "(无 pak/xml)")
        # 报告输出
        if len(sys.argv) > 2:
            with open(sys.argv[2], "w") as f:
                f.write(f"# 基座分析报告 {os.path.basename(ipa)}\n")
                f.write(f"exe={exe}\n")
                f.write(f"deps={deps}\n")
                f.write(f"symbols={syms}\n")
            print(f"报告已写: {sys.argv[2]}")
    finally:
        shutil.rmtree(work, ignore_errors=True)

if __name__ == "__main__":
    main()
