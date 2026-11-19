#!/usr/bin/env python3
# unity_min.py — 最小 Unity assets 解析器（零依赖）
# 目标：列对象类型统计 + 提取 m_Name / TextAsset 内容，不依赖 UnityPy
# 用法: python3 tools/unity_min.py <file.assets> [--dump-text ID] [--json out]
import struct, sys, json

CLASS_NAMES = {1: "GameObject", 43: "Mesh", 48: "Shader", 49: "TextAsset",
               74: "AnimationClip", 83: "AudioClip", 114: "MonoBehaviour",
               213: "Sprite", 210: "Material", 282: "Texture2D", 156: "RectTransform",
               224: "Canvas", 223: "CanvasGroup", 4: "Transform", 141: "BuildSettings"}

class Reader:
    def __init__(self, data, be=True):
        self.d = data
        self.o = 0
        self.be = be
    def u16(self):
        v = struct.unpack_from(">H" if self.be else "<H", self.d, self.o)[0]; self.o += 2; return v
    def u32(self):
        v = struct.unpack_from(">I" if self.be else "<I", self.d, self.o)[0]; self.o += 4; return v
    def i32(self):
        v = struct.unpack_from(">i" if self.be else "<i", self.d, self.o)[0]; self.o += 4; return v
    def i64(self):
        v = struct.unpack_from(">q" if self.be else "<q", self.d, self.o)[0]; self.o += 8; return v
    def u64(self):
        v = struct.unpack_from(">Q" if self.be else "<Q", self.d, self.o)[0]; self.o += 8; return v
    def u8(self):
        v = self.d[self.o]; self.o += 1; return v
    def align4(self):
        self.o = (self.o + 3) & ~3
    def cstr(self):
        e = self.d.index(b"\0", self.o); s = self.d[self.o:e].decode("utf-8", "ignore"); self.o = e + 1; return s
    def sized_str(self):
        n = self.i32()
        if n < 0 or n > 10_000_000: raise ValueError("bad string len %d" % n)
        s = self.d[self.o:self.o + n]; self.o += n; self.align4(); return s

def parse(data):
    """返回 (endianness_be, version, types[{class_id}], objects[{path_id, offset, size, type_id}])"""
    r = Reader(data, be=True)
    metadata_size = r.u32()
    if metadata_size > len(data):
        raise ValueError("bad header")
    # 试 v21+ 布局：fileSize u64, version u32, dataOffset u64
    r2 = Reader(data, be=True)
    r2.u32()
    file_size64 = r2.u64()
    ver = r2.u32()
    if ver >= 21:
        r = r2
        r.u64()  # dataOffset
    else:
        file_size = r.u32()       # fileSize
        ver = r.u32()             # version
        r.u32()                   # dataOffset
    be = bool(r.u8()); r.o += 3   # reserved
    if not be:
        r.be = False
    unity_version = r.cstr()
    r.i32()                        # targetPlatform
    has_tt = bool(r.u8())
    n_types = r.i32()
    types = []
    for _ in range(n_types):
        class_id = r.i32()
        _skip = bool(r.u8())       # hasTypeTree/stripped flag（不解析 tree）
        # 老版本 class_id==-1 时跟 script id
        types.append(class_id)
    r.i32()                        # unknown / scriptTypes count（v17+ 恒 0）
    n_objs = r.i32()
    objs = []
    for _ in range(n_objs):
        r.align4()
        path_id = r.i64()
        if ver >= 21:
            offset = r.u64(); size = r.u64()
        else:
            offset = r.u32(); size = r.u32()
        type_id = r.i32()
        objs.append({"path_id": path_id, "offset": offset, "size": size, "type_id": type_id})
    return be, ver, unity_version, types, objs

# 对象数据开头: m_GameObject PPtr(i32+i64=12) + m_Enabled(u8+3pad) + m_Script PPtr(12) => 28
NAME_OFFSET_FIXED = 28

def read_name(data, off, class_id):
    try:
        r = Reader(data[off:], be=True)
        if class_id == 1:  # GameObject: components vector + layer + name
            n = r.i32()
            if n < 0 or n > 64: return None
            r.o += n * 12
            r.align4()
            r.u32()  # layer
            return r.sized_str().decode("utf-8", "ignore")
        elif class_id in (282, 213, 49, 114, 210, 83, 74, 43, 48):
            r.o = NAME_OFFSET_FIXED
            return r.sized_str().decode("utf-8", "ignore")
    except Exception:
        return None
    return None

def read_text(data, off, size):
    try:
        r = Reader(data[off:off + size], be=True)
        r.o = NAME_OFFSET_FIXED + 4
        n = struct.unpack_from(">i", r.d, r.o)[0]
        if n < 0 or n > 50_000_000: return None
        return r.d[r.o + 4:r.o + 4 + n]
    except Exception:
        return None

def main():
    path = sys.argv[1]
    data = open(path, "rb").read()
    be, ver, uver, types, objs = parse(data)
    from collections import Counter
    id2name = {i: CLASS_NAMES.get(i, str(i)) for i in set(types)}
    # object 的 type_id 引用 type 表下标还是 class_id？ serialized file 中 object 的 type_id
    # 是 type 表索引（v17+ 也是索引）。做映射：按对象顺序对齐风险大，用统计方式兼容两种。
    cnt_idx = Counter(o["type_id"] for o in objs)
    print(f"file={path}")
    print(f"unity={uver} ver={ver} endian={'BE' if be else 'LE'} types={len(types)} objects={len(objs)}")
    print("类型统计(按 type 表索引/class_id 双视图):")
    for tid, c in cnt_idx.most_common(12):
        via_index = id2name.get(tid) if tid < len(types) else "?"
        via_class = CLASS_NAMES.get(tid, str(tid))
        print(f"  id={tid:<6} {c:>6}  as-index={via_index}  as-class={via_class}")
    # 名字采样（用 as-class 视图猜 class_id）
    for tid, c in cnt_idx.most_common(6):
        cls = types[tid] if tid < len(types) else tid
        names = []
        for o in objs:
            if o["type_id"] != tid: continue
            n = read_name(data, o["offset"], cls if cls in CLASS_NAMES else tid)
            names.append(n or "?")
            if len(names) >= 12: break
        print(f"  names[{tid}]:", names)
    # TextAsset 全量 dump
    if "--dump-text" in sys.argv:
        for o in objs:
            tid = o["type_id"]
            cls = types[tid] if tid < len(types) else tid
            if cls == 49 or tid == 49:
                txt = read_text(data, o["offset"], o["size"])
                nm = read_name(data, o["offset"], 49)
                if txt:
                    fn = f"/tmp/textasset_{o['path_id']}_{(nm or 'x').replace('/','_')}.bin"
                    open(fn, "wb").write(txt)
                    print(f"dumped {nm} -> {fn} ({len(txt)}B)")

if __name__ == "__main__":
    main()
