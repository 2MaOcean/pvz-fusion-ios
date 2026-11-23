#!/usr/bin/env python3
# fix_virtual_all.py v2 — 全工程基类全虚化（一次到位，不依赖错误驱动）
# 策略：所有类的实例方法加 virtual（virtual 不改变运行行为）；
#       override 声明沿链向上：根基类有同名方法→已覆盖合法；
#       追到继承链顶（无同名方法/引擎）→ 删除 override（接口实现/引擎魔法方法错加）。
# 用法: python3 fix_virtual_all.py <工程根目录>
import re, os, sys

root = sys.argv[1] if len(sys.argv) > 1 else '.'

files = []
for d in [os.path.join(root, 'Assets', 'Scripts'), os.path.join(root, 'Assets', 'CoreSrc')]:
    if not os.path.isdir(d):
        continue
    for dp, _, fns in os.walk(d):
        for fn in fns:
            if fn.endswith('.cs'):
                files.append(os.path.join(dp, fn))

print(f"扫描 {len(files)} 个 .cs ...")

srcs = {}
def get_src(p):
    if p not in srcs:
        try:
            srcs[p] = open(p, encoding='utf-8', errors='ignore').read()
        except Exception:
            srcs[p] = ''
    return srcs[p]

# ---------- 1. 类索引：class → (file, base) ----------
class_def = {}
class_base = {}
for p in files:
    src = get_src(p)
    if not src:
        continue
    for m in re.finditer(r'\bclass\s+([A-Za-z_]\w*)(?:<[^>]*>)?\s*(?::\s*([^\{{\n]+))?', src):
        cn = m.group(1)
        if cn not in class_def:
            class_def[cn] = p
            b = m.group(2)
            if b:
                bm = re.match(r'\s*([A-Za-z_][\w.]*)', b)
                if bm:
                    class_base[cn] = bm.group(1).split('.')[-1]

print(f"索引到 {len(class_def)} 个类")

# 引擎/系统基类集合（不可修改）
ENGINE_BASES = {'MonoBehaviour', 'ScriptableObject', 'object', 'Object', 'Component',
                'Behaviour', 'AnimatorStateBehaviour', 'StateBehaviour'}

# ---------- 2. 第一遍：所有实例方法加 virtual（跳过已有修饰、构造、static）----------
# 方法声明：修饰符 + 可选 new/virtual/override/... + 返回类型 + 名字 + (
# 排除：static、已有 virtual/override/abstract、构造（名字=类名）、析构
METHOD_PAT = re.compile(
    r'^(\s*)((?:\[[^\]]*\]\s*)?(?:public|protected|internal|private)'
    r'(?:(?:\s+static)|(?:\s+sealed)|(?:\s+extern)|(?:\s+new)|(?:\s+unsafe)|(?:\s+virtual)|(?:\s+override)|(?:\s+abstract))*)'
    r'(\s+)([A-Za-z_][\w<>\[\],.?:\s]*?)\s+([A-Za-z_]\w*)\s*<*\s*(?=\(|\{|=>|;)',
    re.M)

n_virtual_added = 0
for p in files:
    src = get_src(p)
    if 'class' not in src:
        continue
    lines = src.split('\n')
    cur_cls = None
    cur_kind = 'class'
    changed = False
    for i, line in enumerate(lines):
        cm = re.search(r'\b(class|struct|interface)\s+([A-Za-z_]\w*)', line)
        if cm:
            cur_cls = cm.group(2)
            cur_kind = cm.group(1)
        if cur_kind != 'class':
            continue  # struct/interface 成员不能 virtual
        cm2 = METHOD_PAT.search(line)
        if not cm2:
            continue
        mods_full = cm2.group(2)
        meth = cm2.group(5)
        if meth == cur_cls:
            continue  # 构造
        if re.search(r'\b(static|virtual|override|abstract|const|event)\b', mods_full):
            continue  # 已有修饰，不动
        if re.search(r'\bprivate\b', mods_full):
            continue  # private 不能 virtual
        lines[i] = re.sub(
            r'(\b(?:public|protected|internal)\b)',
            r'\1 virtual', line, count=1)
        changed = True
        n_virtual_added += 1
    if changed:
        srcs[p] = '\n'.join(lines)
        open(p, 'w', encoding='utf-8').write(srcs[p])

print(f"第一遍: 加 virtual {n_virtual_added} 个方法")

# ---------- 3. 第二遍：处理 override 声明（沿链向上；顶无同名 → 删 override）----------
overrides = []  # (cls, meth, file)
seen = set()
for p in files:
    src = get_src(p)
    if 'override' not in src:
        continue
    cur_cls = None
    for line in src.split('\n'):
        cm = re.search(r'\bclass\s+([A-Za-z_]\w*)', line)
        if cm:
            cur_cls = cm.group(1)
            continue
        om = re.search(r'\boverride\s+(?:[\w<>\[\],.?:]+?)\s+([A-Za-z_]\w*)', line)
        if om and cur_cls:
            key = (cur_cls, om.group(1))
            if key not in seen:
                seen.add(key)
                overrides.append((cur_cls, om.group(1), p))

print(f"发现 {len(overrides)} 个 override 声明")

def has_member_upstream(start_cls, meth, max_depth=10):
    """沿继承链向上找同名成员；普通方法当场加 virtual（级联）；返回 True 找到"""
    cls = start_cls
    depth = 0
    while cls and depth < max_depth:
        base = class_base.get(cls)
        if not base:
            return False
        if base in ENGINE_BASES:
            return False
        bfile = class_def.get(base)
        if not bfile:
            return False
        bsrc = get_src(bfile)
        # 基类里有没有同名成员（方法或属性）
        if re.search(r'\b' + re.escape(meth) + r'\s*(?:\(|\{|=>)', bsrc):
            # 普通方法 → 当场加 virtual（保证派生类 override 合法）
            bsrc2, cnt = re.subn(
                r'(\b(?:public|protected|internal)\b)(\s+)((?!virtual\b|abstract\b|static\b|override\b)[\w<>,\s]*?\s*)'
                + r'(\b' + re.escape(meth) + r'\b)',
                r'\1 virtual\2\4', bsrc, count=1)
            if cnt and bsrc2 != bsrc:
                srcs[bfile] = bsrc2
                written.add(bfile)
            return True
        cls = base
        depth += 1
    return False

stats = {'keep': 0, 'del': 0, 'nofile': 0}
written = set()
for cls, meth, p in overrides:
    # 基类链上有同名成员 → override 合法（保留）
    if has_member_upstream(cls, meth):
        stats['keep'] += 1
        continue
    # 链上没有 → 反编译错加的 override → 删除关键字
    src = get_src(p)
    cur_cls = None
    new_lines = []
    changed = False
    for line in src.split('\n'):
        cm = re.search(r'\bclass\s+([A-Za-z_]\w*)', line)
        if cm:
            cur_cls = cm.group(1)
        if cur_cls == cls and re.search(r'\boverride\s+', line) and re.search(
                r'\boverride\s+(?:[\w<>\[\],.?:]+?)\s+' + re.escape(meth) + r'\b', line):
            line = re.sub(r'\boverride\s+', '', line, count=1)
            changed = True
            stats['del'] += 1
        new_lines.append(line)
    if changed:
        srcs[p] = '\n'.join(new_lines)
        written.add(p)
    else:
        stats['nofile'] += 1

for p in written:
    open(p, 'w', encoding='utf-8').write(srcs[p])

print(f"完成: 保留合法 override={stats['keep']}, 删除错加 override={stats['del']}, "
      f"无文件匹配={stats['nofile']}, 写文件={len(written)}")
