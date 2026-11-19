#!/usr/bin/env python3
# list_assets.py — Unity 资源清单分析（对象类型统计 + 命名样本）
# 用法: python3 tools/list_assets.py <解包后的 .app 目录> [输出json]
import os, sys, json
from collections import Counter, defaultdict
import UnityPy

def main():
    app = sys.argv[1] if len(sys.argv) > 1 else "."
    data_dir = os.path.join(app, "Data")
    result = {}
    files = []
    for fn in sorted(os.listdir(data_dir)):
        p = os.path.join(data_dir, fn)
        if fn.endswith((".assets", ".asset", ".resource", "sharedassets")) or \
           fn.startswith(("level", "sharedassets")) or fn in ("globalgamemanagers", "globalgamemanagers.assets"):
            files.append(p)
    for p in files:
        name = os.path.basename(p)
        if os.path.isdir(p) or os.path.getsize(p) < 64:
            continue
        try:
            env = UnityPy.load(p)
        except Exception as e:
            result[name] = {"error": str(e)[:120]}
            continue
        types = Counter()
        names = defaultdict(list)
        for obj in env.objects:
            t = obj.type.name
            types[t] += 1
            if len(names[t]) < 25:
                try:
                    d = obj.read()
                    n = getattr(d, "m_Name", None) or getattr(d, "name", "")
                    if n:
                        names[t].append(n)
                except Exception:
                    pass
        result[name] = {"size_mb": round(os.path.getsize(p) / 1048576, 1),
                        "types": dict(types.most_common(15)),
                        "name_samples": {k: v[:12] for k, v in names.items() if k in
                                         ("TextAsset", "MonoBehaviour", "Sprite", "Texture2D", "GameObject")}}
        print(f"== {name} ({result[name]['size_mb']} MB)")
        for t, c in types.most_common(10):
            sample = ", ".join(names[t][:6])
            print(f"   {t:<18} {c:>6}  {sample}")
        if len(types) > 10:
            print(f"   ... 共 {len(types)} 种类型")
    if len(sys.argv) > 2:
        with open(sys.argv[2], "w") as f:
            json.dump(result, f, ensure_ascii=False, indent=1)
        print("已写:", sys.argv[2])

if __name__ == "__main__":
    main()
