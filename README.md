# PVZ Fusion → iOS / iPadOS 移植 + 修改器插件

植物大战僵尸融合版（PvZ Fusion，潜艇伟伟迷 / 蓝飘飘fly 团队）最新版的
iOS / iPadOS 移植工程，附带一个**免越狱可用的修改器 dylib**。

> ⚠️ 仅供个人学习研究。融合版包含 PopCap/EA 的版权素材，
> 请勿公开分发 ipa / 资源包，本仓库只包含代码与脚本。

## 当前状态（2026-11-20 交接）

| 项 | 状态 |
|----|------|
| 修改器 dylib（Unity il2cpp 反射式，版本无关） | ✅ v0.3.2 已发布（[Releases]） |
| 基座 2.0.4b1 分析（Unity 2022.3 il2cpp 复刻版） | ✅ 完成，见 port/PLAN-baseline.md |
| 可安装 ipa（基座+修改器+阳光锁定 9990） | ✅ 已产出（本地产出，不入库） |
| 真机验证（GameAPP.Sun 字段名确认） | ⏳ 等测试者反馈 pvztweak.log |
| 升级到融合版最新版（3.9+）内容 | ⏳ 等新版 iOS 基座或 PC 最新版资源包 |

### 继续此项目的操作路径

1. **验证修改器**：TrollStore/Sideloadly 装包 → 看阳光是否 9990；
   不生效则读取 App 沙盒 `Documents/pvztweak.log`，按内容调整 Offsets.plist
2. **升级内容**：拿到 PC 最新版后，对比 LawnStrings JSON 植物清单差异，
   新增条目补 JSON + 对应贴图（resources.assets/resS 替换，UnityPy 工具链见 tools/）
3. **新增行为**：il2cpp AOT 限制下，新植物复用相似行为类 + 换资源；
   全新机制需 hook（TrollStore 下可写 __TEXT）

### 本机开发环境备忘（DSH 手机端）

- UnityPy：源码包解压至 /opt/pylibs，`PYTHONPATH=/opt/pylibs` 运行
- zig 交叉编译：`pip install ziglang`，`bash tweak/build.sh`
- GitHub 通道：api.github.com + ssh.github.com:443（主站直连不通）

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
     `python3 packaging/inject_dylib.py base.ipa PvzFusionTweak.dylib out.ipa`
     （脚本在 Linux/macOS 均可运行，安装时由 Sideloadly/TrollStore 完成重签）
3. 首次打开需信任开发者证书：设置 → 通用 → VPN与设备管理

## 开发者构建

- 任意平台（推荐，无需 Mac/iOS SDK）：
  ```bash
  pip install ziglang        # zig 交叉工具链
  bash tweak/build.sh        # 产出 tweak/build/PvzFusionTweak.dylib
  ```
  原理：`zig cc -target aarch64-macos -dynamiclib -undefined dynamic_lookup`，
  系统框架符号在 iOS 运行时由 dyld 绑定（no-SDK 构建）。
- macOS + Xcode：同样 `bash tweak/build.sh` 自动走 Xcode 路径。
- 纯云端：推送后 `Actions → Build tweak & package → Run workflow`，
  ubuntu runner 完成 zig 交叉编译 + ldid 伪签 + artifact/Release。

> 注：本仓库曾因 GitHub 对账号的 macOS runner 风控限制改用 ubuntu + zig 方案，
> 若你的账号可用 macOS runner，可把 workflow 的 `runs-on` 改回 `macos-15`。

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
