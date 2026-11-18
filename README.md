# PVZ Fusion → iOS / iPadOS 移植 + 修改器插件

植物大战僵尸融合版（PvZ Fusion，潜艇伟伟迷 / 蓝飘飘fly 团队）最新版的
iOS / iPadOS 移植工程，附带一个**免越狱可用的修改器 dylib**。

> ⚠️ 仅供个人学习研究。融合版包含 PopCap/EA 的版权素材，
> 请勿公开分发 ipa / 资源包，本仓库只包含代码与脚本。

## 现实说明（先读这个）

融合版本体是 **Windows 32 位程序**（基于原版 PvZ 引擎魔改，闭源），
"移植到 iOS" 没有一键方案，本工程采用**双路线**：

| 路线 | 内容 | 现状 |
|------|------|------|
| A. 引擎移植 | 用 PvZ 反编译引擎源码适配 iOS（Metal/OpenGL ES 渲染、触摸映射、pak 资源加载），再逐功能还原融合版逻辑 | 长期工程，见 [port/README.md](port/README.md) |
| B. 修改器先行 | 先产出 dylib 修改器 + ipa 打包流水线，等基座移植版可用后即可注入 | ✅ 本仓库已实现 |

## 修改器 dylib 功能

- 阳光锁定 / 自定义（默认 9990）
- 金币修改
- 卡片冷却清零
- 无冷却施肥 / 铲子不消耗（可选开关）
- 全部开关由 `Offsets.plist` 配置驱动，适配不同版本无需改代码

dylib 设计为**非越狱侧载**场景：通过 [insert_dylib] 打进 ipa，
随 App 启动自动加载，不依赖 MobileSubstrate / Substitute。

## 快速开始（用户视角）

1. 到 [Releases] 下载 `PvzFusionTweak.dylib`（或带基座的 ipa）
2. 电脑上用 [Sideloadly] / AltStore / TrollStore 把 ipa 装进 iPhone/iPad
   - 或：把 dylib 注入你自己已有的 PvZ iOS 基座 ipa：
     `./packaging/make_ipa.sh base.ipa PvzFusionTweak.dylib out.ipa`
3. 首次打开需信任开发者证书：设置 → 通用 → VPN与设备管理

## 开发者构建

- 本地（需要 macOS + Xcode）：
  ```bash
  cd tweak && make          # 产出 build/PvzFusionTweak.dylib
  ```
- 无 Mac：推送到 GitHub 后
  `Actions → Build tweak → Run workflow`，
  macOS runner 云端编译并上传 artifact / Release。

## 目录结构

```
tweak/        修改器 dylib 源码（C + arm64 inline hook）
packaging/    ipa 注入与重签脚本
port/         路线A：引擎移植文档与补丁
.github/      GitHub Actions 云端构建
```

[Releases]: https://github.com/OWNER/pvz-fusion-ios/releases
[Sideloadly]: https://sideloadly.io
[insert_dylib]: https://github.com/Tyilo/insert_dylib
