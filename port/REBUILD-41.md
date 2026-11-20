# 自建 4.1 iOS 移植：完整工程指南

目标：从 PC 4.1 构建产物逆向还原 Unity 工程，构建出 iOS 版。
分工：本机（DSH 环境）完成逆向还原与资产准备；Unity/iOS 构建在有电脑的一侧完成。

## 路线总览

```
PC 4.1 构建产物                 本机（ARM64 Linux）              用户电脑
┌──────────────────┐   AssetRipper   ┌────────────────┐   Unity 2021.3.9f1   ┌─────────────┐
│ PlantsVsZombiesRH.exe │ ──────────→ │ ExportedProject │ ──────────────────→ │ iOS Build    │
│ GameAssembly.dll      │  逆向还原   │ (C#脚本+资产)   │  修错 → Build iOS   │ → Xcode 编译 │
│ data.unity3d          │             └────────────────┘                      └─────────────┘
│ global-metadata.dat   │              ↑ 本机完成                              ↑ 需电脑
└──────────────────┘
```

## 阶段 1：AssetRipper 逆向（本机执行 ✅）

- 工具：AssetRipper 2.0.0 linux_arm64（自带 .NET 运行时）
- 输入：`game41/`（PC 4.1 完整目录，GameAssembly.dll + global-metadata.dat 同位放置，
  AssetRipper 会自动识别 il2cpp 并反编译 C#）
- 输出：`ExportedProject/`（Unity 2021.3.9 工程结构）
- 预计：磁盘 ~6-10GB，耗时 30-90 分钟（441MB bundle、22 万对象）

## 阶段 2：工程修复（用户电脑 / 或指导逐步做）

Unity 逆向的常见损伤，按优先级修复：
1. **编译错误**：反编译 C# 不可编译处（通常 <10 处：out/ref 装箱、泛型约束、unsafe），
   按游戏语义手工修
2. **程序集引用**：缺失 DLL 用 Unity 安装目录的对应托管 DLL 补齐
3. **Shader**：il2cpp 游戏的 ShaderLab 逆向常丢——先跑起来再修渲染问题
4. **引用 GUID**：用 GUID Corrector 类工具校准（ningengit/GUIDcorrector）

## 阶段 3：iOS 构建（用户电脑，需 Mac）

1. 装 **Unity 2021.3.9f1**（必须与原构建同版本，unity.com/releases/editor/archive）
2. 打开 ExportedProject → Build Settings → 平台切换 **iOS**
3. Build 出 Xcode 工程 → Mac 上 Xcode 打开
4. **关键设置**：
   - Signing：个人 Apple ID（免费 7 天）或开发者证书
   - Target → UnityFramework → General：最低 iOS 版本按基座（10.0+）
   - 不要勾 Bitcode
5. 真机运行 → 产出 ipa → TrollStore/Sideloadly 安装

## 阶段 4：修改器接入

iOS 4.1 出来后：本仓库 v0.3.2 修改器是反射式（il2cpp API 运行时定位），
若 4.1 的 GameAPP.Sun 字段名未变则**零改动生效**；有变改 Offsets.plist 的 unity 配置即可。

## 已知风险

- 逆向工程首次编译大概率几十个错误——属正常，逐个修
- Unity 2021.3.9f1 需要匹配的 Unity License（Personal 免费）
- iOS 构建必须 Mac（Unity 不再支持 Windows 出 iOS Xcode 工程以外的完整流程；
  Windows 只能出到 Xcode 工程源，编译仍需 Mac）
- 若无 Mac：可试 Unity 云构建服务（付费）或租 Mac 云主机
