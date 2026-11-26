# PVZ Fusion 4.1 → iOS / Android 移植 + 游戏内修改器

植物大战僵尸融合版（PvZ Fusion，潜艇伟伟迷 / 蓝飘飘fly 团队）**4.1 版**
的 iOS / Android 移植工程，附带**游戏内悬浮菜单修改器**。

> ⚠️ 仅供个人学习研究。融合版包含 PopCap/EA 的版权素材，
> 请勿公开分发 ipa/apk/资源包，本仓库只包含代码与脚本。

## 📦 最新发布

| 版本 | 内容 | 下载 |
|------|------|------|
| **v1.3.0 iOS** | 带游戏内悬浮菜单（修改器 v0.6） | [Releases › v1.3.0](https://github.com/2MaOcean/pvz-fusion-ios/releases/tag/v1.3.0) |
| v1.0.0 iOS | 干净基座（无修改器） | [Releases › v1.0.0](https://github.com/2MaOcean/pvz-fusion-ios/releases/tag/v1.0.0) |
| Android APK | 游戏本体（Android 构建中） | [Releases › android-build](https://github.com/2MaOcean/pvz-fusion-ios/releases/tag/android-build) |

iOS 安装：TrollStore（免签永久）→ 分享 ipa → Install。
未越狱无 TrollStore 可用 AltStore/Sideloadly 侧载（7 天重签）。

## 🎮 修改器功能（游戏内悬浮球，可拖动）

启动游戏后屏幕左侧出现 **PvZ 悬浮球**，点击弹出菜单：

- ☀️ / 💰 阳光、金币锁定（点一下开/关）
- 🌱 全卡池（任意选卡，Board.seedPool 全植物）
- 🪴 任意放置（豌豆/火炬树上鼠标，点哪放哪）
- 💾 立即保存存档（静态字段 → SavePlayerData 落盘）
- 🧟 模式跳转：我是僵尸 1/2 关、冒险、主菜单、宝藏、塔防、挑战
  （`UIMgr.EnterIZGame / EnterGame / EnterMainMenu ...` 运行时反射调用）

高级配置（解包 ipa 改 `Offsets.plist`）：任意地址内存补丁（`mem` 段）、
模式自动跳转、定时存档、probe 字段摸底。日志前缀 `[PvZTweak]`。

## 🏗️ 构建体系（全自动 GitHub Actions）

| Workflow | 产物 | 说明 |
|----------|------|------|
| `rebuild-b23.yml` | iOS ipa | Unity 2022.3.62f1 + il2cpp → xcodebuild |
| `rebuild-android.yml` | Android apk | 同工程 Android target，keystore 签名 |
| `tweak-build.yml` | 修改器 dylib | iOS arm64/arm64e，含悬浮菜单 UI |

**工程要点**（踩坑 30+ 轮的结晶）：
- PC 版 il2cpp 逆向（Il2CppDumper）→ DummyDll 签名 + AssetRipper 源码
- 官方包（URP/TMP/2D/Burst/ugui）+ 第三方源码（spine 4.1 / UniTask / SimpleFileBrowser）
  以 player define 正常编译（**不拷 Editor 编译的 dll**——UnityLinker 会崩）
- 反编译 virtual 丢失 → 全量虚化器 + 20 类错误驱动修复器（幂等，迭代收敛）
- DummyDll 的 `Il2CppDummyDll` 特性会让 UnityLinker 崩溃 → 全部清除，
  Core 框架用 stub 源码真实化（`Singleton<T>` 等）
- 修改器：il2cpp C API（`runtime_invoke` + 静态/实例字段读写）版本无关

## 🗂️ 仓库结构

```
tweak/               修改器源码（C + ObjC 悬浮菜单）
packaging/           ipa 注入打包（inject_dylib.py / make_ipa.sh）
tools/               编译错误修复器（fix_compile_errors.py 等）
.github/workflows/   三个构建 workflow
port/                （早期引擎移植路线存档）
```

## 🧪 修改器开发

```bash
cd tweak && bash build.sh        # 本地 zig 交叉编译（no-SDK）
# CI：Actions › tweak-build.yml（SDK 编译 + ldid 签 + 发 release）
python3 packaging/inject_dylib.py base.ipa tweak.dylib out.ipa
```

配置驱动：`tweak/assets/Offsets.plist`（锁定字段 / 内存补丁 / 模式跳转 /
probe 类清单）。改完提交 → Actions 自动出新 dylib。

## 📜 版本历史

- **v1.3.0**（2026-11-26）游戏内悬浮菜单（7 类模式跳转 + 锁定开关 + 任意放置）
- **v1.2.0** 模式跳转 / 全卡池 / 任意放置 / 存档落盘（plist 驱动）
- **v1.1.0** 修改器 v0.4 内置（静态锁定 + 内存补丁表）
- **v1.0.0** 首个正式版（0 编译错误全链路：il2cpp → xcodebuild → ipa）
- v0.3.x 早期 dylib（旧 2.0.4b1 基座，已弃）
