# 移植作业计划：以 iOS 基座 → 融合版最新版

前提：用户提供一个社区移植的 PvZ iOS 版 ipa 作为**引擎基座**。
目标：在其上叠加融合版最新版的资源与逻辑，产出完整可玩 ipa。

## 第 0 步：基座判定（拿到 ipa 立即做）

```bash
python3 tools/analyze_base.py <base.ipa> base_report.md
```

判定维度：
- **引擎类型**：Lawn 原版引擎 / SDL / cocos2d / GameMaker / Unity
- **符号状况**：未 strip（可 dlsym 适配修改器）/ strip（需逆向偏移）
- **资源形态**：main.pak / 散装 png / properties
- **架构与版本**：arm64、最低 iOS、是否 PIE、是否加密

## 第 1 步：融合版素材准备（用户提供或从 PC 版提取）

- `main.pak`（融合版资源包）或解包后的散资源
- `properties/` 配置（植物/僵尸/关卡参数）
- 版本号记录（对照最新版发布说明）

## 第 2 步：资源层替换

- 若基座用 pak：直接替换/合并 pak（保留基座必需的平台资源）
- 若基座散装：写 pak→散装 转换脚本（tools/）
- 分辨率对齐：基座渲染基准分辨率 vs 融合版 800x600 逻辑分辨率

## 第 3 步：逻辑层差距评估（关键）

融合版相对原版的改动分两类：

| 类型 | 例子 | 移植难度 |
|------|------|----------|
| 资源驱动 | 新植物/僵尸贴图、音效、关卡数据、图鉴 | 低（换资源即可） |
| 代码驱动 | 融合种植机制、新植物行为 AI、磁力菇吸附等交互 | 高（需逆向基座引擎行为函数或 Lua/脚本扩展） |

代码驱动部分的三种实现路径（拿到基座后按引擎能力选择）：
1. 基座自带脚本扩展点（部分移植版内置 Lua/JSON 行为定义）
2. 基座未 strip：dylib hook 行为函数实现新机制（复用本仓库 tweak/）
3. 全无：二进制级 patch（TrollStore 场景）

## 第 4 步：打包

```bash
python3 packaging/inject_dylib.py base.ipa tweak/build/PvzFusionTweak.dylib PvzFusion-full.ipa
```
Sideloadly/AltStore/TrollStore 安装。

## 里程碑

- [ ] M0 基座分析报告完成
- [ ] M1 融合版资源在基座中加载显示（贴图/音效替换）
- [ ] M2 原版关卡可玩 + 修改器生效
- [ ] M3 融合种植机制可玩
- [ ] M4 新植物/僵尸行为对齐最新版
- [ ] M5 完整版 ipa 发布（个人使用）
