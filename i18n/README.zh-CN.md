<p align="center">
  <img src="../src/launcher/assets/app-icon.png" alt="MCWiiU Launcher" width="112">
</p>

<h1 align="center">MCWiiU Launcher</h1>

<p align="center">在一个启动器中畅玩 Minecraft: Wii U Edition。</p>

<p align="center">
  <strong>0.4.0 Stable — 准备发布</strong><br>
  Windows 10 / 11 x64 · Cemu-based · Unofficial
</p>

<p align="center">
  <a href="../README.md">日本語</a> · <a href="README.en.md">English</a> · <a href="README.ko.md">한국어</a> · <strong>简体中文</strong> · <a href="README.es.md">Español</a>
</p>

<p align="center">
  <a href="../docs/INSTALLATION.md">安装指南</a> · <a href="../docs/FEATURES.md">功能指南</a>
</p>

---

## 简介

MCWiiU Launcher 是一款基于 Cemu 的非官方启动器，用于在 Windows PC 上游玩 Minecraft: Wii U Edition **v560**。从安装游戏到管理账户、世界和资源包，再到启动与更新，都可以在同一个界面中完成。

## 发布准备情况

**首个版本正在准备中。目前没有正式的 GitHub Release，也没有公开的 Setup。** 首次发布计划为 **0.4.0 Stable**，Foundation 为 **1.0.2**，Updater Protocol 为 **2**。

生产环境的 Ed25519 公钥尚未配置。发布前仍需确认图片再分发许可、许可证与对应源代码的最终核对、依赖库的剩余漏洞、干净构建与分发测试，以及在全新 Windows 10 / 11 环境中的验收。当前开发产物不可用于面向公众的分发。

## 主要功能

- **Minecraft Launch / Game Data** — 在 Native 版本中点击 `PLAY`，即可在同一窗口内游玩。支持安装和管理 BASE、同地区的 Update v560 以及可选的 DLC。
- **Account / Friends** — 在 Settings 中管理账户和选择服务，查看 Friends 列表并管理好友申请。可用性取决于服务条款和连接状态。
- **World Manager** — 导入世界、修改名称与图标、在 Account 之间移动世界，以及使用 Trash 和恢复功能。
- **Texture Pack / Graphic Pack Manager** — 验证、选择和配置本地资源包。更改将在下次启动游戏时生效。
- **Session History / Discord Rich Presence** — 查看按日、周、月、年统计的游玩时间和每日记录，分享当前页面与游玩状态。Presence 也受 Discord 共享设置的影响。
- **Launcher Settings / Secure Updates** — 配置输入、图形、音频与 Overlay，并在用户批准后应用带签名的 Launcher 更新。

游戏中按 `F10` 打开结束确认，按 `ESC` 取消，按 `F11` 切换显示模式。详细操作和限制见 [FEATURES](../docs/FEATURES.md)。已实现某项功能并不代表保证兼容所有 PC 或在线服务。

## 系统要求

| 项目 | 要求与条件 |
|---|---|
| OS | Windows 10 / 11 x64 |
| CPU / RAM | x64 CPU。最低 CPU、指令集和内存要求尚未确定 |
| GPU | 支持 Vulkan 或 OpenGL 的 GPU 及合适的驱动程序，优先使用 Vulkan |
| Runtime | Microsoft Edge WebView2 Evergreen Runtime。缺失时 Setup 会停止，不会自动安装 |
| 存储 | 需容纳应用、游戏、MLC、存档、资源包、缓存及安装/更新临时文件；最低容量尚未确定 |

Windows 10 的常规支持已于 **2025-10-14 结束**。列为目标 OS 不代表保证 OS 安全支持。请参阅 [Microsoft 的说明](https://www.microsoft.com/en-us/windows/end-of-support)。

目前的验收主要在同一台 Windows 11 开发主机、隔离的自有 JPN v560 游戏副本及有限的 GPU 环境中进行。Windows 10、全新 PC、所有地区、输入与音频设备以及长时间运行的全面验收尚未完成。不含 Core 的 Standalone 开发版本无法运行游戏。

## 安装

1. 阅读 [INSTALLATION](../docs/INSTALLATION.md)，了解环境要求和安全检查。**目前没有可下载的正式 Setup。**
2. 发布后，请从官方 [GitHub Releases](https://github.com/IGNSeed/MCWiiU-Launcher/releases) 获取 Setup 并选择安装目录。Setup **不使用 Authenticode 签名**。请按照指南独立核实分发来源和文件完整性。
3. 运行 `MCWiiU-Launcher.exe`（Bootstrapper），安装自有游戏数据，然后在 Home 中选择 `PLAY`。程序本体为 `MCWiiU Launcher.exe`，更新程序为 `MCWiiU-Updater.exe`。

从旧开发版本自动迁移不在正式支持范围内。卸载和数据保留方式也请参阅安装指南。

## 游戏数据

**不附带游戏本体、Update、DLC、存档、Wii U 系统文件或游戏密钥。** 您需要自行提供拥有且合法取得的数据。

请使用与 BASE 同地区的 Update **v560**。DLC 为可选内容。[GAME_DATA](../docs/GAME_DATA.md) 说明了安装、更改地区、仅删除游戏数据以及保留存档的方式。操作前请备份重要存档。

## 更新

常规 Launcher 更新会在用户批准后应用带签名的 ZIP，并验证 Ed25519、SHA-256、GitHub Asset 的对应关系和文件清单。若 30 秒 Readiness 检查失败，Rollback 会返回可验证的旧版本。**由于尚未导入生产环境密钥，当前开发版本无法使用正式自动更新。**

Foundation 采用**手动 Setup 更新**，与常规 Launcher 更新分开进行。详情和恢复方式见 [UPDATING](../docs/UPDATING.md)。

## 文档

以下详细指南目前以日语提供。

- [安装与卸载](../docs/INSTALLATION.md)
- [自有游戏数据与存档](../docs/GAME_DATA.md)
- [操作与设置](../docs/FEATURES.md)
- [更新与恢复](../docs/UPDATING.md)
- [故障排除](../docs/TROUBLESHOOTING.md)
- [存储数据与网络通信](../docs/PRIVACY.md)
- [许可证与对应源代码](../docs/THIRD_PARTY_NOTICES.md)
- [分发核实与安全报告](../SECURITY.md)
- [更新日志](../CHANGELOG.md)
- 开发、构建与贡献: [BUILD](../BUILD.md) · [CONTRIBUTING](../CONTRIBUTING.md)

计划使用 GitHub Issues 接收问题反馈，但**目前已禁用，反馈渠道尚未准备完毕**。请勿在公开反馈中包含游戏密钥、令牌、账户信息或存档。

## 致谢

感谢 [Cemu](https://github.com/cemu-project/Cemu) 及其贡献者，以及 saucer、WebView2、Discord Social SDK 和各依赖库的开发者。原始声明及对应源代码说明见 [THIRD_PARTY_NOTICES](../docs/THIRD_PARTY_NOTICES.md)。

## 许可证

保留 Cemu 衍生代码的 **MPL-2.0** 许可证和版权声明。原文见 [LICENSE.txt](../LICENSE.txt)。第三方依赖分别适用各自的条款。源代码许可证不授予游戏资产或第三方图片的再分发许可。

## 免责声明

MCWiiU Launcher 不是 Cemu、Mojang、Microsoft 或 Nintendo 的官方产品，也未由其提供或认可。Minecraft 及相关名称与素材的权利归各权利人所有。
