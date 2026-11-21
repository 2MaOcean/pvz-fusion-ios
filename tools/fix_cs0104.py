#!/usr/bin/env python3
# fix_cs0104.py — 解析 Unity 构建日志的 CS0104 歧义错误，批量加 using alias
# 用法: python3 fix_cs0104.py <unity_build.log> [工程根目录]
import re, os, sys

log = sys.argv[1]
root = sys.argv[2] if len(sys.argv) > 2 else '.'
text = open(log, encoding='utf-8', errors='ignore').read()

files = set()
for m in re.finditer(r'(Assets/[^\s"]+\.cs)\(\d+,\d+\): error CS0104', text):
    files.add(m.group(1))

print(f"CS0104 文件数: {len(files)}")
alias = 'using YieldAwaitable = Cysharp.Threading.Tasks.YieldAwaitable;'
fixed = 0
for rel in sorted(files):
    path = os.path.join(root, rel)
    if not os.path.exists(path):
        print(f"  跳过（不存在）: {rel}")
        continue
    s = open(path, encoding='utf-8', errors='ignore').read()
    if 'using YieldAwaitable = Cysharp.Threading.Tasks' in s:
        continue
    lines = s.split('\n')
    ins = 0
    for i, l in enumerate(lines[:30]):
        if l.strip().startswith('using '):
            ins = i
            break
    lines.insert(ins, alias)
    open(path, 'w', encoding='utf-8').write('\n'.join(lines))
    fixed += 1
    print(f"  fixed: {rel}")
print(f"已修复文件数: {fixed}")
