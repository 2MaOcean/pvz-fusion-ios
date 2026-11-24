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
# Bee 输出截断检测：有编译失败标记但解析不到错误 → 提示（错误消息被 warning 洪水截断）
if not errs and '## Script Compilation Error' in text:
    print("WARN: 检测到 Script Compilation Error 但无 error 行 —— 输出被截断，需抑制 warning（csc.rsp）")

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

# ---------- CS0426：嵌套类型缺失 → 仅记录（v7：禁用文件曾造成 150 个文件误伤）----------
def fix_cs0426():
    files = {f for f, ln, msg in by_code.get('CS0426', [])}
    for rel in sorted(files):
        print(f"  CS0426 (仅记录，不禁用): {rel}")
    return 0

# ---------- CS0246：类型缺失 → 工程内搜定义改引用 / stub ----------
def fix_cs0138():
    """'using X;' 中 X 是类型不是命名空间 → 删除该 using 行"""
    n = 0
    for f, ln, msg in by_code.get('CS0138', []):
        m = re.search(r"'([\w.]+)' is a type not a namespace", msg)
        if not m:
            continue
        t = m.group(1)
        path = os.path.join(root, f)
        if not os.path.exists(path):
            continue
        s = open(path, encoding='utf-8', errors='ignore').read()
        new = re.sub(r'^\s*using\s+' + re.escape(t) + r'\s*;\s*\n', '', s, count=1, flags=re.M)
        if new != s:
            open(path, 'w', encoding='utf-8').write(new)
            n += 1
            print(f"  CS0138: {os.path.basename(f)} 删除 using {t};")
    return n

def fix_cs0246():
    n = 0
    # 提取缺失类型名
    missing_types = {}
    for f, ln, msg in by_code.get('CS0246', []):
        m = re.search(r"The type or namespace name '(\w+)(<>)?'", msg)
        if m: missing_types.setdefault((m.group(1), bool(m.group(2))), set()).add(f)
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
    for (t, generic), ref_files in sorted(missing_types.items()):
        if t in defs:
            # 定义存在：引用文件可能缺 using——跳过（复杂），仅记录
            print(f"  CS0246: {t} 定义存在于 {defs[t][:2]}（需人工检查 using）")
            continue
        # 无定义 → 仅记录（v7：stub 空壳曾把 Plant/Board/UGUI 类型污染成空类，改由
        # workflow 的「源码完整性恢复」步骤从原始包找回真实类型；确实缺失的类型下轮人工 stub）
        print(f"  CS0246 missing (仅记录): {t}{'<T>' if generic else ''} 在 {[os.path.basename(x) for x in sorted(ref_files)][:3]}")
    return n

# ---------- CS0549：sealed 类里的新 virtual 成员 → 改 override（恢复真实重写关系）----------
def fix_cs0549():
    n = 0
    seen = set()
    for f, ln, msg in by_code.get('CS0549', []):
        m = re.match(r"'([\w.]+?)(?:<[^<>]*>)?(?:\([^)]*\))?' is a new virtual member in sealed type '([\w.]+)'", msg)
        if not m:
            continue
        member_full, cls_full = m.group(1), m.group(2)
        meth = member_full.split('.')[-1].split('<')[0]
        if meth == cls_full.split('.')[-1]:
            continue  # 构造器不能 override
        path = os.path.join(root, f)
        if not os.path.exists(path):
            continue
        s = open(path, encoding='utf-8', errors='ignore').read()
        # 定位该成员声明行（含 virtual），virtual → override
        pat = re.compile(
            r'^(\s*(?:\[[^\]]*\]\s*)?(?:public|protected|internal)'
            r'(?:\s+(?:static|sealed|extern|new|unsafe))*)\s+virtual(\s+)'
            r'((?:[\w<>\[\],.?:]+?)\s+)?' + re.escape(meth) +
            r'(\s*(?:<[^<>]*>)?\s*(?:\(|\{|=>))',
            re.M)
        new_s, cnt = pat.subn(r'\1 override\2\3' + meth + r'\4', s, count=1)
        if cnt:
            open(path, 'w', encoding='utf-8').write(new_s)
            n += 1
            print(f"  CS0549: {os.path.basename(f)}.{meth} virtual→override")
    return n

# ---------- CS0534：未实现引擎/基类 abstract 成员 ----------
# 已有成员带 virtual（被误转）→ 改回 override；无成员 → 追加 override 默认实现
def fix_cs0534():
    n = 0
    seen = set()
    for f, ln, msg in by_code.get('CS0534', []):
        m = re.match(r"'(\w+)' does not implement inherited abstract member '([\w.]+?)\.(\w+?)(<[^>]*>)?(\(.*\)|\.get|\.set)'", msg)
        if not m:
            continue
        cls, base, mem = m.group(1), m.group(2), m.group(3)
        tail = m.group(5) or ''
        is_prop = tail in ('.get', '.set')
        key = (cls, mem, is_prop)
        if key in seen:
            continue
        seen.add(key)
        path = os.path.join(root, f)
        if not os.path.exists(path):
            continue
        s = open(path, encoding='utf-8', errors='ignore').read()
        # 1) 成员已在（方法/属性），带 virtual → 改 override
        pat = re.compile(
            r'^(\s*(?:\[[^\]]*\]\s*)?(?:public|protected|internal|private)'
            r'(?:\s+(?:static|sealed|extern|new|unsafe))*)\s+virtual(\s+)'
            r'((?:[\w<>\[\],.?:]+?)\s+)?' + re.escape(mem) +
            r'(\s*(?:<[^<>]*>)?\s*(?:\(|\{|=>))',
            re.M)
        new_s, cnt = pat.subn(r'\1 override\2\3' + mem + r'\4', s, count=1)
        if cnt:
            open(path, 'w', encoding='utf-8').write(new_s)
            n += 1
            print(f"  CS0534: {os.path.basename(f)}.{mem} virtual→override（已有实现）")
            continue
        # 2) 成员已存在但无修饰（普通方法）→ 加 override（仅当方法名可定位）
        if is_prop:
            prop_any = re.search(r'\b' + re.escape(mem) + r'\s*(?:\{|=>|;)', s)
            if prop_any:
                # 属性已存在：普通形式（无 override/virtual 前缀）→ 加 override
                prop_pat = re.compile(
                    r'^(\s*(?:public|protected|internal|private)(?:\s+(?:static|sealed|extern|new|unsafe))*)\s+'
                    r'((?!override\b|virtual\b|abstract\b|async\b|new\b|static\b|sealed\b)([\w<>\[\],.?:]+))\s+'
                    + re.escape(mem) + r'\s*(\{|=>|;)', re.M)
                mprop = prop_pat.search(s)
                if mprop:
                    typ = mprop.group(3)
                    body = 'false' if typ in ('bool', 'System.Boolean') else f'default({typ})'
                    ind = re.match(r'\s*', mprop.group(1)).group(0)
                    decl = f'{ind}public override {typ} {mem} {{ get {{ return {body}; }} }}'
                    new_s = prop_pat.sub(decl.replace('\\', '\\\\'), s, count=1)
                    open(path, 'w', encoding='utf-8').write(new_s)
                    n += 1
                    print(f"  CS0534: {os.path.basename(f)}.{mem} 普通属性→override")
                # 已是 override/virtual 形式 → 不动
                continue
            # 属性不存在 → 追加 override（keepWaiting 等常为 bool）
            ins = _insert_in_class(path, s, cls, f'public override bool {mem} {{ get {{ return false; }} }}')
            if ins:
                n += 1
                print(f"  CS0534: {os.path.basename(f)} 追加 override 属性 {mem}")
            continue
        # 方法：任何形式已存在？
        meth_any = re.search(r'\b' + re.escape(mem) + r'\s*(?:<[^<>]*>)?\s*\(', s)
        if not meth_any:
            # 完全缺失 → 追加 override 默认实现（仅空参；返回类型未知用 void 兜底）
            if tail.strip() == '()':
                ins = _insert_in_class(path, s, cls, f'public override void {mem}() {{ }}')
                if ins:
                    n += 1
                    print(f"  CS0534: {os.path.basename(f)} 追加 override {mem}()")
            continue
        # 存在但非 override/virtual 形式 → 加 override 修饰
        meth_exists = re.search(
            r'^\s*(?:\[[^\]]*\]\s*)?(?:public|protected|internal|private)(?:\s+(?:static|sealed|extern|new|unsafe))*\s+(?!override\b|virtual\b)[^\n(){};=]*\b' + re.escape(mem) + r'\s*(?:<[^<>]*>)?\s*\(', s, re.M)
        if meth_exists:
            meth_pat = re.compile(
                r'^(\s*(?:\[[^\]]*\]\s*)?(?:public|protected|internal|private)'
                r'(?:\s+(?:static|sealed|extern|new|unsafe))*)(\s+)'
                r'((?!override\b|virtual\b)(?:[\w<>\[\],.?:]+?)\s+)(' + re.escape(mem) + r'\s*(?:<[^<>]*>)?\s*\()',
                re.M)
            new_s, cnt = meth_pat.subn(r'\1 override\2\3\4', s, count=1)
            if cnt:
                open(path, 'w', encoding='utf-8').write(new_s)
                n += 1
                print(f"  CS0534: {os.path.basename(f)}.{mem} 普通方法→override")
            continue
        # 3) 完全缺失 → 追加 override 默认实现（仅支持空参方法；返回类型未知用 void/bool 兜底）
        args_empty = tail.strip() == '()'
        if args_empty:
            ret = 'void'
            body = '{ }'
            decl = f'public override {ret} {mem}() {body}'
            ins = _insert_in_class(path, s, cls, decl)
            if ins:
                n += 1
                print(f"  CS0534: {os.path.basename(f)} 追加 override {mem}()")
    return n

def _insert_in_class(path, s, cls, decl):
    """在类 cls 的类体开头插入一行 decl；成功返回 True 并写文件"""
    cm = re.search(r'\bclass\s+' + re.escape(cls) + r'\b[^\{]*\{', s)
    if not cm:
        # 类声明与 { 分离（基类列表在下一行）
        cm2 = re.search(r'\bclass\s+' + re.escape(cls) + r'\b[^\{]*\n[^\{]*\{', s)
        if not cm2:
            return False
        cm = cm2
    pos = cm.end()
    # 缩进取类声明行缩进 + 4
    line_start = s.rfind('\n', 0, cm.start()) + 1
    indent = re.match(r'\s*', s[line_start:]).group(0) + '    '
    new_s = s[:pos] + '\n' + indent + decl + s[pos:]
    open(path, 'w', encoding='utf-8').write(new_s)
    return True

# ---------- CS0117：枚举缺成员 → 给枚举补成员 ----------
def fix_cs0117():
    n = 0
    seen = set()
    for f, ln, msg in by_code.get('CS0117', []):
        m = re.match(r"'(\w+)' does not contain a definition for '(\w+)'", msg)
        if not m:
            continue
        enum, member = m.group(1), m.group(2)
        key = (enum, member)
        if key in seen:
            continue
        seen.add(key)
        # 全工程找枚举定义
        target = None
        idx = _class_index()
        for p in idx.get(enum, []):
            try:
                s = open(p, encoding='utf-8', errors='ignore').read()
            except Exception:
                continue
            em = re.search(r'\benum\s+' + re.escape(enum) + r'\b[^{]*\{', s)
            if em:
                target = (p, s, em.end())
                break
        if not target:
            print(f"  CS0117: {enum} 枚举定义未找到（跳过）")
            continue
        p, s, pos = target
        if re.search(r'\b' + re.escape(member) + r'\b', s):
            continue  # 已有成员（可能别名/其他错误）
        # 插入 Default = 0（值重复合法，不会破坏显式值成员）
        new_s = s[:pos] + f'\n    {member} = 0,' + s[pos:]
        open(p, 'w', encoding='utf-8').write(new_s)
        n += 1
        print(f"  CS0117: {os.path.basename(p)} 枚举 {enum} 补成员 {member} = 0")
    return n

# ---------- CS1721：接口被当成基类（CS0246 stub 生成了 class）→ stub 改 interface ----------
def fix_cs1721():
    n = 0
    seen = set()
    for f, ln, msg in by_code.get('CS1721', []):
        m = re.match(r"Class '\w+' cannot have multiple base classes: '[\w.]+' and '([\w.]+)'", msg)
        if not m:
            continue
        iface = m.group(1).split('.')[-1]
        if iface in seen:
            continue
        seen.add(iface)
        # 找全局 stub class 定义并改为 interface
        sp = os.path.join(root, 'Assets', 'Scripts', 'Stubs', f'Stub_{iface}.cs')
        if not os.path.exists(sp):
            continue
        s = open(sp, encoding='utf-8', errors='ignore').read()
        new_s = re.sub(r'\bpublic\s+class\s+(' + re.escape(iface) + r')\b[^{]*\{',
                       r'public interface \1 {', s, count=1)
        if new_s != s:
            open(sp, 'w', encoding='utf-8').write(new_s)
            n += 1
            print(f"  CS1721: Stub_{iface}.cs class→interface")
    return n

# ---------- CS0106：virtual 修饰符无效 → 删除该行的 virtual ----------
def fix_cs0106():
    n = 0
    seen = set()
    for f, ln, msg in by_code.get('CS0106', []):
        key = (f, ln)
        if key in seen:
            continue
        seen.add(key)
        path = os.path.join(root, f)
        if not os.path.exists(path):
            continue
        lines = open(path, encoding='utf-8', errors='ignore').read().split('\n')
        # 报错行及其邻近（行号可能漂移）找含 virtual 的行
        for d in (0, -1, 1, -2, 2):
            idx = ln - 1 + d
            if 0 <= idx < len(lines) and 'virtual' in lines[idx]:
                lines[idx] = re.sub(r'\bvirtual\s+', '', lines[idx], count=1)
                open(path, 'w', encoding='utf-8').write('\n'.join(lines))
                n += 1
                print(f"  CS0106: {os.path.basename(f)}:{idx+1} 删除 virtual")
                break
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
                for m in re.finditer(r'\b(?:class|struct|interface|enum)\s+([A-Za-z_]\w*)', src):
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
    追到引擎基类 / 链顶（无同名方法）→ 是反编译错加的 override，删除 override 关键字
    （v6：不再限定生命周期方法——Unity 生命周期与游戏虚方法都会被错加）。"""
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
        resolved = False  # True=基类已虚（override 合法）或已加 virtual
        while chain < 8 and cur_file:
            chain += 1
            try:
                src = open(cur_file, encoding='utf-8', errors='ignore').read()
            except Exception:
                break
            mdecl = re.search(
                r'^(\s*)((?:\[[^\]]*\]\s*)?(?:public|protected|internal|private)'
                r'(?:\s+static)?(?:\s+sealed)?(?:\s+extern)?(?:\s+new)?(?:\s+unsafe)?)'
                r'(\s+(?:virtual|override|abstract|static|new)?\s*)'
                r'((?:[\w<>\[\],.?:]+?)\s+)?' + re.escape(meth) + r'\s*<*\s*(?:\(|\{|=>)',
                src, re.M)
            if not mdecl:
                break  # 当前类无此成员 → 链顶 → 删 override
            mods = mdecl.group(2) + (mdecl.group(3) or '')
            if 'virtual' in mods or 'abstract' in mods:
                resolved = True
                break
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
                resolved = True
                break
            # 是 override → 看类基类继续向上
            cm = re.search(r'\bclass\s+' + re.escape(cur_cls) + r'\b[^{]*:\s*([A-Za-z_][\w.]*)', src)
            if not cm:
                break
            nxt = cm.group(1).split('.')[-1]
            if nxt in ('MonoBehaviour', 'ScriptableObject', 'object', 'Object') or nxt == cur_cls:
                break  # 追到引擎 → 链顶 → 删 override
            nxt_file = _find_class_file(nxt, [os.path.dirname(cur_file)])
            if not nxt_file:
                break  # 基类不在源码索引（引擎/第三方/DLL）→ 链顶 → 删 override
            cur_cls, cur_file = nxt, nxt_file
        if not resolved:
            # 追到链顶无同名成员 → 反编译错加的 override → 删除关键字
            esrc = open(os.path.join(root, f), encoding='utf-8', errors='ignore').read()
            pat2 = re.compile(
                r'(\b(?:public|protected|internal|private)\s+)override(\s+(?:[\w<>\[\],.?:]+?)\s+'
                + re.escape(meth) + r'\s*<*\s*(?:\(|\{|=>))')
            new_src, cnt = pat2.subn(r'\1\2', esrc, count=1)
            if cnt:
                open(os.path.join(root, f), 'w', encoding='utf-8').write(new_src)
                n += 1
                print(f"  CS0115: {os.path.basename(f)}.{meth} 删除 override（链顶无同名）")
    return n

if 'CS0104' in by_code: stats.append(('CS0104', fix_cs0104()))
if 'CS0426' in by_code: stats.append(('CS0426', fix_cs0426()))
if 'CS0246' in by_code: stats.append(('CS0246', fix_cs0246()))
if 'CS0115' in by_code: stats.append(('CS0115', fix_cs0115()))
if 'CS0138' in by_code: stats.append(('CS0138', fix_cs0138()))
if 'CS0549' in by_code: stats.append(('CS0549', fix_cs0549()))
if 'CS0534' in by_code: stats.append(('CS0534', fix_cs0534()))
if 'CS0106' in by_code: stats.append(('CS0106', fix_cs0106()))
if 'CS0117' in by_code: stats.append(('CS0117', fix_cs0117()))
if 'CS1721' in by_code: stats.append(('CS1721', fix_cs1721()))

print(f"修复文件数: {len(fixed_files)}")
for code, n in stats:
    print(f"STAT {code} fixed={n}")
