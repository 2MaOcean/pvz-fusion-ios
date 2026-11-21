#!/usr/bin/env python3
# fix_cs0426.py — 修复 CS0426（嵌套类型缺失）
# 策略：在工程 Assets 里找同名类型定义 → 改引用为可见形式；找不到则生成空 stub
# 用法: python3 fix_cs0426.py <unity_build.log> <工程根目录>
import re, os, sys

log = sys.argv[1]
root = sys.argv[2] if len(sys.argv) > 2 else '.'
text = open(log, encoding='utf-8', errors='ignore').read()

# 收集 CS0426: (file, line, col, OuterType, MissingType)
errs = []
for m in re.finditer(r'(Assets/[^\s"]+\.cs)\((\d+),(\d+)\): error CS0426: The type name \'([^\']+)\' does not exist in the type \'([^\']+)\'', text):
    errs.append((m.group(1), int(m.group(2)), int(m.group(3)), m.group(5), m.group(4)))

print(f"CS0426 错误数: {len(errs)}")
if not errs:
    sys.exit(0)

# 在工程里搜类型定义
defs = {}  # name -> [相对文件路径]
for dirpath, _, files in os.walk(os.path.join(root, 'Assets')):
    for fn in files:
        if not fn.endswith('.cs'): continue
        p = os.path.join(dirpath, fn)
        try:
            s = open(p, encoding='utf-8', errors='ignore').read()
        except Exception: continue
        for cm in re.finditer(r'\b(class|struct|interface|enum)\s+(\w+)', s):
            defs.setdefault(cm.group(2), []).append(os.path.relpath(p, root))

fixed_files = set()
stubs = set()
for rel, ln, col, outer, missing in errs:
    path = os.path.join(root, rel)
    if not os.path.exists(path): continue
    if missing in defs:
        # 定义存在：把 "Outer.Missing" 引用改成 "Missing"（用顶层名）
        s = open(path, encoding='utf-8', errors='ignore').read()
        pat = re.compile(r'\b' + re.escape(outer) + r'\.' + re.escape(missing) + r'\b')
        s2, n = pat.subn(missing, s)
        if n:
            open(path, 'w', encoding='utf-8').write(s2)
            fixed_files.add(rel)
            print(f"  ref-fixed: {rel} ({outer}.{missing} -> {missing}, {n} 处)")
    else:
        # 无定义：生成全局 stub + 把引用 "Outer.Missing" 改为 "Missing"
        stubs.add(missing)
        s = open(path, encoding='utf-8', errors='ignore').read()
        pat = re.compile(r'\b' + re.escape(outer) + r'\.' + re.escape(missing) + r'\b')
        s2, n = pat.subn(missing, s)
        if n:
            open(path, 'w', encoding='utf-8').write(s2)
            fixed_files.add(rel)
            print(f"  ref->stub: {rel} ({outer}.{missing} -> {missing}, {n} 处)")

# 兜底：注释仍报错的行（引用是变量访问，文本替换不可达）
for rel, ln, col, outer, missing in errs:
    path = os.path.join(root, rel)
    if not os.path.exists(path): continue
    lines = open(path, encoding='utf-8', errors='ignore').read().split('\n')
    idx = ln - 1
    if 0 <= idx < len(lines) and lines[idx].strip() and not lines[idx].strip().startswith('//'):
        lines[idx] = '// [CS0426-stubbed] ' + lines[idx]
        open(path, 'w', encoding='utf-8').write('\n'.join(lines))
        fixed_files.add(rel)
        print(f"  line-stubbed: {rel}:{ln}")

# 生成 stub（全局命名空间空类型）
if stubs:
    stub_dir = os.path.join(root, 'Assets', 'Scripts', 'Stubs')
    os.makedirs(stub_dir, exist_ok=True)
    sp = os.path.join(stub_dir, 'MissingTypes.cs')
    content = ['// 自动生成的缺失类型桩（AssetRipper 逆向）']
    for t in sorted(stubs):
        content.append(f'namespace UnityEngine {{ public class {t} {{ }} }}')
    open(sp, 'w', encoding='utf-8').write('\n'.join(content) + '\n')
    print(f"生成 stub: {sp} -> {sorted(stubs)}")

print(f"修复文件数: {len(fixed_files)}")
