# 路线 A：融合版引擎移植 iOS 技术方案

## 目标二进制构成

融合版 = 魔改的 `PlantsVsZombies.exe`（Win32 x86, PopCap Lawn 引擎）+ 资源包 `main.pak`。

## 移植分层

```
┌────────────────────────────────────────┐
│ 融合版玩法层（闭源，需逆向还原）         │  ← 逐功能移植：融合格、杂交植物、
│ (LawnBoard 魔改、新 Zombie/Plant 类)    │     关卡编辑器、商店改版…
├────────────────────────────────────────┤
│ Lawn 引擎层（社区反编译源码可获取）      │  ← 编译目标换 arm64 + iOS
│ Sexy.AppBase / Sexy.Widget / Lawn.*    │
├────────────────────────────────────────┤
│ 平台抽象层                              │  ← 重写四个子系统
└────────────────────────────────────────┘
```

## 平台抽象层替换清单

| 子系统 | Win32 原实现 | iOS 替代 |
|--------|--------------|----------|
| 渲染 | DirectDraw / D3D9 | OpenGL ES 3 / Metal（贴图取 BGRA→RGBA） |
| 音频 | DirectSound | OpenAL / AVAudioPlayer |
| 输入 | WM_MOUSE* | UITouch → 坐标缩放（基准 800x600） |
| 文件 | 裸路径 + registry | 沙盒 Documents + NSUserDefaults |
| 计时 | QueryPerformanceCounter | mach_absolute_time |
| 弹窗 | MessageBox | UIAlertController 桥接 |

## 关键工程点

1. **宽高比**：原版固定 800x600，iPad 4:3 最合适；iPhone 加黑边或 zoom 模式。
2. **Reanimator**：Reanimation 播放逻辑为纯 CPU 计算，可直接复用，注意浮点行为差异（x87 80-bit → ARM64 32/64-bit），涉及粒子轨迹的要做回归比对。
3. **存档**：原版 user.dat 二进制结构保留，转存 Documents，可兼容 PC 存档导入。
4. **pak 加载**：Lawn 的 ResourceManager 直接支持 pak，只需替换文件定位函数到沙盒路径。
5. **融合版逻辑还原**：以 Windows 版为参照（Cheat Engine + x64dbg 动态分析 + 常量扫描），
   优先级：融合种植 > 新植物/僵尸 > 小游戏 > 图鉴。

## 里程碑

- [ ] M1 引擎源码在 iOS 模拟器跑通进入主菜单
- [ ] M2 真机 60fps 渲染 + 触摸操作可玩原版关卡
- [ ] M3 融合种植核心机制
- [ ] M4 融合版内容量对齐最新版 90%
- [ ] M5 修改器 dylib（本仓库 tweak/）适配融合作弊项
