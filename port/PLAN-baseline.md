# 移植作业计划：以 iOS 基座 → 融合版最新版

前提：用户提供一个社区移植的 PvZ iOS 版 ipa 作为**引擎基座**。
目标：在其上叠加融合版最新版的资源与逻辑，产出完整可玩 ipa。

## 第 0 步：基座判定（已完成 ✅ 2026-11-20）

基座：`植物大战僵尸 融合版_2.0.4b1[ 星].ipa`（161MB，用户提供）

| 维度 | 结果 |
|------|------|
| 引擎 | **Unity 2022.3.20f1c1 + il2cpp**（AOT 原生码） |
| 主程序 | 壳 88KB + UnityFramework 48MB（引擎+游戏逻辑） |
| Bundle | com.GLESign.PlantsVsZombiesRH，min iOS 10，TrollStore 分发（TrollFools 注入痕迹） |
| 自带修改器 | H5GG 变种 `Unlimted Sun.dylib`（Web 覆盖 UI + Substrate hook） |
| 资源结构 | resources.assets 33MB（37694 对象）+ resS 318MB（纹理音频流）+ resources.resource 23MB（音频） |
| 文本表 | **LawnStrings = JSON**（106 植物图鉴，富文本，数据驱动） |
| 脚本组件 | MonoScript 1587 全提取；MonoBehaviour 类名分布：AlmanacCard/CardUI/Advanture_Btn/TallNutFootball 等 UI+玩法组件 |
| 字节序 | **serialized file 为 little-endian**（Unity 2022 特性，解析时必须注意） |
| 数值层 | 待确认：LawnStrings 仅图鉴文本；真实数值在 il2cpp 代码或未定位的配置（下一步） |

### 解析工具链（本机已验证可用）
```bash
# UnityPy 绕过编译：源码包解压 + 依赖补丁（详见仓库 tools/）
PYTHONPATH=/opt/pylibs python3 tools/list_assets.py <解包的.app>
python3 tools/analyze_base.py <base.ipa>
```

## 4.1 PC 版分析结论（2026-11-20，用户提供 PC 4.1 包后完成）

样本：`植物大战僵尸融合版4.1`（PC Windows 版，1.1GB）

| 维度 | PC 4.1 | iOS 基座 2.0.4b1 | 差异影响 |
|------|--------|------------------|----------|
| Unity 引擎 | **2021.3.9f1** | 2022.3.20f1c1 | 序列化格式不同，资源不可直接互换 |
| il2cpp 元数据 | global-metadata.dat 14MB | 6.7MB | 代码量约 2 倍 |
| LawnStrings | JSON **751KB** | 74.5KB | 图鉴 742 vs 106 条植物 |
| data 布局 | data.unity3d 单一压缩 bundle | 散装 assets | 加载路径不同 |
| 平台 | Windows x64 (GameAssembly.dll) | iOS arm64 (UnityFramework) | 逻辑二进制不可迁移 |

### 内容差异规模
- 4.1 图鉴 **742 种植物**（2.0.4b1 仅 106——2.0.4 是早期版，连磁力菇/玉米投手等原版植物都没有）
- 新增 636+ 植物行为类（各一个 .cs）已 AOT 编译进 PC 版 GameAssembly.dll

### 升级可行性结论（重要）
1. **PC 4.1 资源无法灌入 iOS 2.0.4 基座**：Unity 版本（2021 vs 2022）+ data 布局 + il2cpp 引擎三重不匹配
2. **新增植物行为代码无法注入**：il2cpp 是 AOT 预编译，636 个新 C# 类不在 iOS arm64 二进制里
3. **官方无 4.1 iOS 版**（游民星空 4.1 手机版原文：iOS 只能在线玩；社区最新为 2.0.4）
4. → 「iOS 原生玩 4.1」仅剩两条路：
   - **官方网页版**（iOS 浏览器直玩，官方提供的在线方式）
   - **自建 iOS 移植**：需 Mac + Unity 2021.3.9 + AssetRipper 逆向重建工程后出 iOS 构建（月级工程，本工程可提供资产准备与指南，但构建必须在 Mac 完成）
5. 当前 **2.0.4b1 基座是 iOS 原生可玩的最高版本**，本工程的反射修改器对其完全有效

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

## 第 4 步：打包（已验证可产出 ✅ 2026-11-20）

实际流水线（本机已跑通）：
1. 解包基座 ipa → 删除旧 H5GG 修改器（`Unlimted Sun.dylib` / `Tg@TrollStoreKios.dylib`），
   用 `.troll-fools.bak` 恢复原始主二进制
2. 复制 `tweak/build/PvzFusionTweak.dylib` 与 `tweak/assets/Offsets.plist` 进 .app
3. `python3 packaging/inject_dylib.py` 注入 LC_LOAD_DYLIB（验证：引用存在、ncmds 递增）
4. 重打包 ipa → TrollStore / Sideloadly 安装（安装器自动重签，无需本地 ldid）

产出：`PvzFusion-iOS-v0.3.1-tweak.ipa`（151MB，阳光锁定 9990 配置预置）

## 里程碑

- [ ] M0 基座分析报告完成
- [ ] M1 融合版资源在基座中加载显示（贴图/音效替换）
- [ ] M2 原版关卡可玩 + 修改器生效
- [ ] M3 融合种植机制可玩
- [ ] M4 新植物/僵尸行为对齐最新版
- [ ] M5 完整版 ipa 发布（个人使用）
