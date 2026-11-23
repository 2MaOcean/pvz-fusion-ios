#!/usr/bin/env python3
# fix_virtual_all.py — 全量修复反编译源码丢失的 virtual（一次性，不依赖错误驱动）
# 用法: python3 fix_virtual_all.py <工程根目录>
import re, os, sys

root = sys.argv[1] if len(sys.argv) > 1 else '.'

LIFECYCLE = {
    'Awake', 'Start', 'Update', 'FixedUpdate', 'LateUpdate', 'OnEnable', 'OnDisable',
    'OnDestroy', 'OnApplicationPause', 'OnApplicationQuit', 'OnGUI', 'OnDrawGizmos',
    'OnDrawGizmosSelected', 'OnValidate', 'Reset', 'OnApplicationFocus',
    'OnTriggerEnter', 'OnTriggerExit', 'OnTriggerStay',
    'OnCollisionEnter', 'OnCollisionExit', 'OnCollisionStay',
}

# ---------- 1. 扫描全部 .cs：class→(file, base)，文件内容缓存 ----------
files = []          # 所有 .cs 路径
srcs = {}           # path → 内容（懒加载缓存）
class_def = {}      # 类名 → 文件路径
class_base = {}     # 类名 → 基类名

for d in [os.path.join(root, 'Assets', 'Scripts'), os.path.join(root, 'Assets', 'CoreSrc')]:
    if not os.path.isdir(d):
        continue
    for dp, _, fns in os.walk(d):
        for fn in fns:
            if fn.endswith('.cs'):
                files.append(os.path.join(dp, fn))

print(f"扫描 {len(files)} 个 .cs ...")

def get_src(p):
    if p not in srcs:
        try:
            srcs[p] = open(p, encoding='utf-8', errors='ignore').read()
        except Exception:
            srcs[p] = ''
    return srcs[p]

for p in files:
    src = get_src(p)
    if not src:
        continue
    for m in re.finditer(r'\bclass\s+([A-Za-z_]\w*)(?:<[^>]*>)?\s*(?::\s*([^\{{]+))?', src):
        cn = m.group(1)
        if cn not in class_def:
            class_def[cn] = p
            b = m.group(2)
            if b:
                bm = re.match(r'\s*([A-Za-z_][\w.]*)', b)
                if bm:
                    class_base[cn] = bm.group(1).split('.')[-1]

print(f"索引到 {len(class_def)} 个类")

# 方法声明模式（捕获修饰与 virtual/override 状态）
DECL_PAT = re.compile(
    r'^(\s*)((?:\[[^\]]*\]\s*)?(?:public|protected|internal|private)'
    r'(?:\s+static)?(?:\s+sealed)?(?:\s+extern)?(?:\s+new)?(?:\s+unsafe)?)'
    r'(\s+(virtual|override|abstract|static|new|extern|unsafe)\s*|\s+)'
    r'((?:[\w<>\[\],.?:]+?)\s+)?' + r'({meth})' + r'\s*<*\s*\(',
    re.M)

def method_kind(src, cls, meth):
    """返回 (kind, match) — kind: none/virtual/override/abstract/normal"""
    # 找类定义区间（简化：整个文件搜，多类文件可能误中——取类声明后的第一个匹配）
    ci = src.find('class ' + cls)
    if ci < 0:
        return 'none', None
    region = src[ci:]
    mm = DECL_PAT.replace('{meth}', re.escape(meth)).search(region) if False else re.search(
        r'^(\s*)((?:\[[^\]]*\]\s*)?(?:public|protected|internal|private)'
        r'(?:\s+static)?(?:\s+sealed)?(?:\s+extern)?(?:\s+new)?(?:\s+unsafe)?)'
        r'(\s+(virtual|override|abstract|static|new|extern|unsafe)\s*|\s+)'
        r'((?:[\w<>\[\],.?:]+?)\s+)?' + re.escape(meth) + r'\s*<*\s*\(',
        region, re.M)
    if not mm:
        return 'none', None
    kw = mm.group(3) or ''
    if 'virtual' in kw:
        return 'virtual', mm
    if 'override' in kw:
        return 'override', mm
    if 'abstract' in kw:
        return 'abstract', mm
    if 'static' in kw:
        return 'static', mm
    return 'normal', mm

# ---------- 2. 收集全部 override 方法（行级状态机：记录最近 class 声明）----------
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
        om = re.search(r'\boverride\s+(?:[\w<>\[\],.?:]+?)\s+([A-Za-z_]\w*)\s*<*\s*\(', line)
        if om and cur_cls:
            key = (cur_cls, om.group(1))
            if key not in seen:
                seen.add(key)
                overrides.append((cur_cls, om.group(1), p))

print(f"发现 {len(overrides)} 个 override 方法")

# ---------- 3. 逐个沿链修复 ----------
stats = {'root_virtual': 0, 'del_override': 0, 'already': 0, 'skip': 0, 'nofile': 0}
written = set()

def flush(p):
    if p in written:
        open(p, 'w', encoding='utf-8').write(srcs[p])

for cls, meth, err_file in overrides:
    cur_cls, cur_file = cls, class_def.get(cls, err_file)
    chain = 0
    while chain < 10:
        chain += 1
        src = get_src(cur_file)
        kind, mm = method_kind(src, cur_cls, meth)
        if kind == 'none':
            # 中间层无此方法声明 → 继续向基类找
            base = class_base.get(cur_cls)
            if base and base != cur_cls and base not in ('MonoBehaviour', 'ScriptableObject', 'object', 'Object') and chain < 10:
                nxt_file = class_def.get(base)
                if nxt_file:
                    cur_cls, cur_file = base, nxt_file
                    continue
            # 追到引擎基类：生命周期方法删 override（override 声明在最初的类文件里）
            if meth in LIFECYCLE:
                origin = None
                for c2, m2, f2 in overrides:
                    if c2 == cls and m2 == meth:
                        origin = f2
                        break
                if origin:
                    osrc = get_src(origin)
                    pat2 = re.compile(
                        r'(\b(?:public|protected|internal|private)\s+)override(\s+(?:[\w<>\[\],.?:]+?)\s+)'
                        + re.escape(meth) + r'\s*<*\s*\(')
                    src2, cnt = pat2.subn(r'\1\2' + meth + '(', osrc, count=1)
                    if cnt:
                        srcs[origin] = src2
                        written.add(origin)
                        stats['del_override'] += 1
                        break
            stats['skip'] += 1
            break
        if kind == 'virtual' or kind == 'abstract':
            stats['already'] += 1
            break
        if kind == 'normal':
            # 加 virtual
            reg = src.find('class ' + cur_cls)
            head, tail = src[:reg], src[reg:]
            pat = re.compile(
                r'^(\s*)((?:\[[^\]]*\]\s*)?(?:public|protected|internal|private)'
                r'(?:\s+static)?(?:\s+sealed)?(?:\s+extern)?(?:\s+new)?(?:\s+unsafe)?)'
                r'(\s+(?!virtual\b|abstract\b|static\b|override\b|extern\b|partial\b))'
                r'((?:[\w<>\[\],.?:]+?)\s+)?' + re.escape(meth) + r'\s*<*\s*\(', re.M)
            def _addv(m2):
                ind, mo, rest = m2.group(1), m2.group(2).strip(), m2.group(4) or ''
                return f"{ind}{mo} virtual {rest}{meth}("
            tail, cnt = pat.subn(_addv, tail, count=1)
            if cnt:
                srcs[cur_file] = head + tail
                written.add(cur_file)
                stats['root_virtual'] += 1
            break
        if kind == 'override':
            base = class_base.get(cur_cls)
            if not base or base in ('MonoBehaviour', 'ScriptableObject', 'object', 'Object', cur_cls):
                # 追到引擎：仅生命周期方法删 override
                if meth in LIFECYCLE:
                    pat2 = re.compile(
                        r'(\b(?:public|protected|internal|private)\s+)override(\s+(?:[\w<>\[\],.?:]+?)\s+)'
                        + re.escape(meth) + r'\s*<*\s*\(')
                    src2, cnt = pat2.subn(r'\1\2' + meth + '(', src, count=1)
                    if cnt:
                        srcs[cur_file] = src2
                        written.add(cur_file)
                        stats['del_override'] += 1
                else:
                    stats['skip'] += 1
                break
            nxt_file = class_def.get(base)
            if not nxt_file:
                stats['nofile'] += 1
                break
            cur_cls, cur_file = base, nxt_file
            continue

for p in written:
    flush(p)

print(f"完成: 根基类加 virtual={stats['root_virtual']}, 删生命周期 override={stats['del_override']}, "
      f"已是virtual={stats['already']}, 跳过={stats['skip']}, 基类文件缺失={stats['nofile']}, 写文件={len(written)}")
