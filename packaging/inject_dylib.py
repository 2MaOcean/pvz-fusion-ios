#!/usr/bin/env python3
# inject_dylib.py — 把修改器 dylib 注入基座 ipa（Linux/macOS 通用，无需 insert_dylib）
# 用法: python3 inject_dylib.py <base.ipa> <tweak.dylib> <out.ipa>
# 原理: 解包 ipa → 主二进制 load commands 末尾追加 LC_LOAD_DYLIB → ldid 重签 → 重打包
# 依赖: ldid（PATH 中）；重签也可以由 Sideloadly/AltStore/TrollStore 在安装时完成
import os, shutil, struct, subprocess, sys, tempfile, zipfile

CMD_SIZE = 8

def find_app_dir(payload):
    for n in os.listdir(payload):
        if n.endswith(".app"):
            return os.path.join(payload, n)
    raise SystemExit("错误: ipa 中未找到 .app")

def read_cstr(buf, off):
    e = buf.index(b"\x00", off)
    return buf[off:e].decode("utf-8", "ignore")

def inject_dylib_path(bin_path, dylib_name):
    """在主二进制 load commands 末尾追加 LC_LOAD_DYLIB"""
    with open(bin_path, "rb") as f:
        data = bytearray(f.read())
    magic, cputype, _, filetype, ncmds, sizeofcmds, flags = struct.unpack_from("<IiiIII I", data, 0)
    if magic != 0xFEEDFACF:
        raise SystemExit(f"错误: {bin_path} 不是 64 位 Mach-O")
    if cputype & 0xFFFFFF != 12:
        print(f"警告: cputype={cputype} 非 arm64")

    lc_entry = 32  # mach_header_64
    end = lc_entry + sizeofcmds
    # LC_LOAD_DYLIB: cmd(4) cmdsize(4) name_ofs(4) timestamp(4) current_version(4)
    #                compat_version(4) + dylib name string
    name_b = dylib_name.encode() + b"\x00"
    lc_size = 24 + len(name_b)
    lc_size = (lc_size + 7) & ~7  # 8 字节对齐
    lc = struct.pack("<IIIIII", 0xC, lc_size, 24, 1, 0x10000, 0x10000) + name_b
    lc += b"\x00" * (lc_size - len(lc))

    # 检查 load commands 区末尾到第一个 section 数据之间是否有空隙（arm64 主二进制通常有 0x4000 对齐的余量）
    first_sec_off = None
    off = lc_entry
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, off)
        if cmd == 0x19:  # LC_SEGMENT_64，找第一个有数据的 section
            nsects = struct.unpack_from("<I", data, off + 64)[0]
            if nsects:
                s0 = struct.unpack_from("<I", data, off + 72 + 48)[0]  # section[0].offset
                if s0 and (first_sec_off is None or s0 < first_sec_off):
                    first_sec_off = s0
        off += size
    room = (first_sec_off or len(data)) - end
    if lc_size > room:
        raise SystemExit(f"错误: load commands 区空间不足（需 {lc_size}，余 {room}），需用 insert_dylib 平移")

    data[end:end] = lc
    struct.pack_into("<I", data, 16, ncmds + 1)   # ncmds
    struct.pack_into("<I", data, 20, sizeofcmds + lc_size)  # sizeofcmds
    with open(bin_path, "wb") as f:
        f.write(data)
    print(f"已注入 @r/{dylib_name} -> {bin_path} (LC_LOAD_DYLIB, {lc_size}B)")

def main():
    if len(sys.argv) != 4:
        print(__doc__); sys.exit(1)
    base_ipa, dylib, out_ipa = sys.argv[1:4]

    work = tempfile.mkdtemp()
    try:
        with zipfile.ZipFile(base_ipa) as z:
            z.extractall(work)
        app = find_app_dir(os.path.join(work, "Payload"))
        exe = subprocess.check_output(
            ["plutil", "-extract", "CFBundleExecutable", "raw", "-o", "-", f"{app}/Info.plist"],
            text=True).strip() if shutil.which("plutil") else None
        if not exe:  # 手工解析二进制 plist 或默认取 app 名
            exe = os.path.basename(app)[:-4]
            print(f"提示: 未解析到 CFBundleExecutable，使用默认 {exe}")
        shutil.copy(dylib, os.path.join(app, os.path.basename(dylib)))
        inject_dylib_path(os.path.join(app, exe), "@r/" + os.path.basename(dylib))
        if shutil.which("ldid"):
            subprocess.check_call(["ldid", "-S", os.path.join(app, exe)])
            subprocess.check_call(["ldid", "-S", os.path.join(app, os.path.basename(dylib))])
        else:
            print("警告: 无 ldid；请用 Sideloadly/AltStore/TrollStore 安装时重签")
        if os.path.exists(out_ipa):
            os.remove(out_ipa)
        with zipfile.ZipFile(out_ipa, "w", zipfile.ZIP_DEFLATED) as z:
            for root, _, files in os.walk(work):
                for fn in files:
                    p = os.path.join(root, fn)
                    z.write(p, os.path.relpath(p, work))
        print(f"完成: {out_ipa}")
    finally:
        shutil.rmtree(work, ignore_errors=True)

if __name__ == "__main__":
    main()
