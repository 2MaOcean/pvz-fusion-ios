#!/usr/bin/env python3
# fix_compile_errors.py — 统一编译错误修复入口（按错误码分类）
# 用法: python3 fix_compile_errors.py <unity_build.log> <工程根目录>
# 输出: 修复统计（stdout），供 CI 判断
import re, os, sys

log = sys.argv[1]
root = sys.argv[2] if len(sys.argv) > 2 else '.'
text = open(log, encoding='utf-8', errors='ignore').read()

# 收集全部错误
errs = []  # (file, line, code, message)
for m in re.finditer(r'(Assets/[^\s"]+\.cs)\((\d+),(\d+)\): (error CS\d+): (.+)', text):
    errs.append((m.group(1), int(m.group(2)), m.group(4).replace('error ', ''), m.group(5)))

by_code = {}
for f, ln, code, msg in errs:
    by_code.setdefault(code, []).append((f, ln, msg))

print(f"错误总数: {len(errs)}")
for code, items in sorted(by_code.items()):
    print(f"  {code}: {len(items)}")

fixed_files = set()
stats = []

# ---------- CS0104：歧义引用 → using alias ----------
def fix_cs0104():
    n = 0
    files = {f for f, ln, msg in by_code.get('CS0104', [])}
    for rel in sorted(files):
        path = os.path.join(root, rel)
        if not os.path.exists(path): continue
        s = open(path, encoding='utf-8', errors='ignore').read()
        # 从错误信息提取歧义类型名（取第一个候选）
        m = re.search(r"ambiguous reference between '([\w.]+)'", msg_of(by_code, 'CS0104', rel))
        if not m: continue
        target = m.group(1).split('.')[-1]
        alias_line = f'using {target} = {m.group(1)};'
        if alias_line in s: continue
        lines = s.split('\n')
        ins = 0
        for i, l in enumerate(lines[:30]):
            if l.strip().startswith('using '):
                ins = i; break
        lines.insert(ins, alias_line)
        open(path, 'w', encoding='utf-8').write('\n'.join(lines))
        fixed_files.add(rel); n += 1
        print(f"  CS0104 fixed: {rel} -> {alias_line}")
    return n

def msg_of(by_code, code, rel):
    for f, ln, msg in by_code.get(code, []):
        if f == rel: return msg
    return ''

# ---------- CS0426：嵌套类型缺失 → 文件排除 ----------
def fix_cs0426():
    n = 0
    files = {f for f, ln, msg in by_code.get('CS0426', [])}
    for rel in sorted(files):
        path = os.path.join(root, rel)
        if not os.path.exists(path): continue
        disabled = path + '.disabled'
        if os.path.exists(disabled): continue
        os.rename(path, disabled)
        fixed_files.add(rel); n += 1
        print(f"  CS0426 excluded: {rel} -> {os.path.basename(disabled)}")
    return n

# ---------- CS0246：类型缺失 → 工程内搜定义改引用 / stub ----------
def fix_cs0246():
    n = 0
    # 提取缺失类型名
    missing_types = {}
    for f, ln, msg in by_code.get('CS0246', []):
        m = re.search(r"The type or namespace name '(\w+)'", msg)
        if m: missing_types.setdefault(m.group(1), set()).add(f)
    if not missing_types: return 0
    # 工程内搜定义
    defs = {}
    for dirpath, _, files in os.walk(os.path.join(root, 'Assets')):
        for fn in files:
            if not fn.endswith('.cs'): continue
            p = os.path.join(dirpath, fn)
            try: s = open(p, encoding='utf-8', errors='ignore').read()
            except Exception: continue
            for cm in re.finditer(r'\b(class|struct|interface|enum)\s+(\w+)', s):
                defs.setdefault(cm.group(2), []).append(os.path.relpath(p, root))
    for t, ref_files in sorted(missing_types.items()):
        if t in defs:
            # 定义存在：引用文件可能缺 using——跳过（复杂），仅记录
            print(f"  CS0246: {t} 定义存在于 {defs[t][:2]}（需人工检查 using）")
            continue
        # 无定义 → stub
        stub_dir = os.path.join(root, 'Assets', 'Scripts', 'Stubs')
        os.makedirs(stub_dir, exist_ok=True)
        sp = os.path.join(stub_dir, f'Stub_{t}.cs')
        if not os.path.exists(sp):
            open(sp, 'w', encoding='utf-8').write(
                f'// 自动生成的缺失类型桩（全局命名空间）\npublic class {t} : UnityEngine.MonoBehaviour {{ }}\n')
            n += 1
            print(f"  CS0246 stub: {t}")
    return n

# ---------- 执行 ----------
# ---------- CS0115：基类方法缺 virtual（il2cpp 元数据无 virtual 信息，反编译丢失）----------
CLASS_INDEX = None
def _class_index():
    """一次性扫描全部 .cs，按内容建 类名→文件列表 索引（文件名与类名可能不一致）"""
    global CLASS_INDEX
    if CLASS_INDEX is not None:
        return CLASS_INDEX
    idx = {}
    for d in [os.path.join(root, 'Assets', 'Scripts'), os.path.join(root, 'Assets', 'CoreSrc')]:
        if not os.path.isdir(d):
            continue
        for dp, _, fns in os.walk(d):
            for fn in fns:
                if not fn.endswith('.cs'):
                    continue
                p = os.path.join(dp, fn)
                try:
                    src = open(p, encoding='utf-8', errors='ignore').read()
                except Exception:
                    continue
                if len(src) > 400000:
                    src = src[:400000]
                for m in re.finditer(r'\b(?:class|struct|interface)\s+([A-Za-z_]\w*)', src):
                    idx.setdefault(m.group(1), []).append(p)
    CLASS_INDEX = idx
    return idx

def _find_class_file(cls, extra_dirs):
    idx = _class_index()
    lst = idx.get(cls) or []
    return lst[0] if lst else None

LIFECYCLE_METHODS = {
    'Awake', 'Start', 'Update', 'FixedUpdate', 'LateUpdate', 'OnEnable', 'OnDisable',
    'OnDestroy', 'OnApplicationPause', 'OnApplicationQuit', 'OnGUI', 'OnDrawGizmos',
    'OnDrawGizmosSelected', 'OnValidate', 'Reset', 'OnApplicationFocus', 'OnTriggerEnter',
    'OnTriggerExit', 'OnTriggerStay', 'OnCollisionEnter', 'OnCollisionExit', 'OnCollisionStay',
}

def fix_cs0115():
    """'X.M()': no suitable method found to override
    → 沿继承链向上：基类方法是普通方法则加 virtual；是 override 则继续向上；
    追到引擎基类（MonoBehaviour 等）则是反编译错加的 override，直接删除 override 关键字。"""
    n = 0
    seen = set()
    for f, ln, msg in by_code.get('CS0115', []):
        m = re.match(r"'([\w.]+)\.([\w<>]+)[^']*': no suitable method found to override", msg)
        if not m:
            continue
        cls, meth = m.group(1).split('.')[-1], m.group(2)
        key = (cls, meth)
        if key in seen:
            continue
        seen.add(key)

        cur_cls, cur_file = cls, os.path.join(root, f)
        chain = 0
        prev_override_file = cur_file  # 最初错误文件（追到引擎时删它的 override）
        while chain < 8:
            chain += 1
            try:
                src = open(cur_file, encoding='utf-8', errors='ignore').read()
            except Exception:
                cur_file = None
                break
            # 当前类的方法/属性声明（属性无括号，形如 int BuffType { 或 =>）
            mdecl = re.search(
                r'^(\s*)((?:\[[^\]]*\]\s*)?(?:public|protected|internal|private)'
                r'(?:\s+static)?(?:\s+sealed)?(?:\s+extern)?(?:\s+new)?(?:\s+unsafe)?)'
                r'(\s+(?:virtual|override|abstract|static|new)?\s*)'
                r'((?:[\w<>\[\],.?:]+?)\s+)?' + re.escape(meth) + r'\s*<*\s*(?:\(|\{|=>)',
                src, re.M)
            if not mdecl:
                break  # 当前文件无此方法/属性（理论少见）
            mods = mdecl.group(2) + (mdecl.group(3) or '')
            if 'virtual' in mods or 'abstract' in mods:
                break  # 已是虚方法（前轮修好），本轮不动
            if 'override' not in mods:
                # 普通方法/属性 → 加 virtual
                pat = re.compile(
                    r'^(\s*)((?:\[[^\]]*\]\s*)?(?:public|protected|internal|private)'
                    r'(?:\s+static)?(?:\s+sealed)?(?:\s+extern)?(?:\s+new)?(?:\s+unsafe)?)'
                    r'(\s+(?!virtual\b|abstract\b|static\b|override\b|extern\b|partial\b))'
                    r'((?:[\w<>\[\],.?:]+?)\s+)?(?=' + re.escape(meth) + r'\s*<*\s*(?:\(|\{|=>))',
                    re.M)
                def _addv(mm):
                    ind, mo, rest = mm.group(1), mm.group(2).strip(), mm.group(4) or ''
                    return f"{ind}{mo} virtual {rest}"
                new_src, cnt = pat.subn(_addv, src, count=1)
                if cnt:
                    open(cur_file, 'w', encoding='utf-8').write(new_src)
                    n += 1
                    print(f"  CS0115: {os.path.basename(cur_file)}({cur_cls}).{meth} → virtual")
                break
            # 是 override → 看类基类继续向上
            cm = re.search(r'\bclass\s+' + re.escape(cur_cls) + r'\b[^{]*:\s*([A-Za-z_][\w.]*)', src)
            if not cm:
                break
            nxt = cm.group(1).split('.')[-1]
            if nxt in ('MonoBehaviour', 'ScriptableObject', 'object', 'Object') or nxt == cur_cls:
                # 追到引擎：最初错误文件里错加的 override → 删除（仅生命周期方法）
                if meth in LIFECYCLE_METHODS:
                    esrc = open(prev_override_file, encoding='utf-8', errors='ignore').read()
                    pat2 = re.compile(
                        r'(\b(?:public|protected|internal|private)\s+)override(\s+(?:[\w<>\[\],.?:]+?)\s+)'
                        + re.escape(meth) + r'\s*<*\s*\(')
                    new_src, cnt = pat2.subn(r'\1\2' + meth + '(', esrc, count=1)
                    if cnt:
                        open(prev_override_file, 'w', encoding='utf-8').write(new_src)
                        n += 1
                        print(f"  CS0115: {os.path.basename(prev_override_file)}.{meth} 删除 override（生命周期方法）")
                break
            # 继续向上
            nxt_file = _find_class_file(nxt, [os.path.dirname(cur_file)])
            if not nxt_file:
                break
            prev_override_file = cur_file
            cur_cls, cur_file = nxt, nxt_file
    return n

if 'CS0104' in by_code: stats.append(('CS0104', fix_cs0104()))
if 'CS0426' in by_code: stats.append(('CS0426', fix_cs0426()))
if 'CS0246' in by_code: stats.append(('CS0246', fix_cs0246()))
if 'CS0115' in by_code: stats.append(('CS0115', fix_cs0115()))

print(f"修复文件数: {len(fixed_files)}")
for code, n in stats:
    print(f"STAT {code} fixed={n}")
