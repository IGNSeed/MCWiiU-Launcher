<p align="center">
  <img src="../src/launcher/assets/app-icon.png" alt="MCWiiU Launcher" width="112">
</p>

<h1 align="center">MCWiiU Launcher</h1>

<p align="center">Minecraft: Wii U Edition, together in one launcher.</p>

<p align="center">
  <strong>0.4.3 Stable — Released</strong><br>
  Windows 10 / 11 x64 · Cemu-based · Unofficial
</p>

<p align="center">
  <a href="../README.md">日本語</a> · <strong>English</strong> · <a href="README.ko.md">한국어</a> · <a href="README.zh-CN.md">简体中文</a> · <a href="README.es.md">Español</a>
</p>

<p align="center">
  <a href="../docs/INSTALLATION.md">Installation guide</a> · <a href="../docs/FEATURES.md">Feature guide</a>
</p>

---

## Overview

MCWiiU Launcher is an unofficial, Cemu-based launcher for Minecraft: Wii U Edition **v560** on Windows PCs. Install your game, manage accounts, worlds and packs, then play and update from one interface.

## Release status

v0.4.3 improves exit-button stability and adds a black-and-white software keyboard with physical keyboard input, Windows Japanese IME, Unicode rendering and centered text. Shader progress UI is hidden while caching/compilation remain enabled. Duplicate DOM/IPC work, hidden-game rendering and Vulkan submissions are reduced; Windows Native Vulkan uses a CPU-saving hybrid wait. While playing, list/refresh worlds, open their folder, import, rename and move them to Trash. Modifying/deleting a loaded world while it is being saved is not guaranteed safe. Existing JPN Mii initialization remains. Reduced launcher-animation smoothness during OpenGL gameplay is unresolved. [ZIP](https://github.com/IGNSeed/MCWiiU-Launcher/releases/download/v0.4.3/MCWiiU-Launcher-0.4.3-win-x64.zip)

**[0.4.3 Stable](https://github.com/IGNSeed/MCWiiU-Launcher/releases/tag/v0.4.3) is available.** Foundation **1.0.2**, Updater Protocol **2**. [Download Setup](https://github.com/IGNSeed/MCWiiU-Launcher/releases/download/v0.4.3/MCWiiU-Launcher-0.4.3-Setup-x64.exe).

Release binary source commit: [3d9339b09fae4f587b12acb5fed8a8e0a679f9d3](https://github.com/IGNSeed/MCWiiU-Launcher/commit/3d9339b09fae4f587b12acb5fed8a8e0a679f9d3).

The production Ed25519 public key is embedded; signed manifest integrity and public downloads have been verified. Image redistribution rights, open license and corresponding-source checks, remaining dependency vulnerabilities, and full acceptance on clean Windows 10 / 11 systems remain incomplete.

## Features

- **Minecraft Launch / Game Data** — Use `PLAY` in the Native build to play in the same window. Install and manage BASE, the matching region's Update v560 and optional DLC.
- **Account / Friends** — Manage accounts and select services in Settings; view friends and manage requests. Availability depends on service terms and connectivity.
- **World Manager** — Import worlds, change names and icons, move worlds between Accounts, and use Trash and restore.
- **Texture Pack / Graphic Pack Manager** — Validate, select and configure local packs. Changes apply at the next game launch.
- **Session History / Discord Rich Presence** — View daily, weekly, monthly and yearly playtime and dated history; share the current page or play state. Presence also depends on Discord's sharing settings.
- **Launcher Settings / Secure Updates** — Configure input, graphics, audio and Overlay, with signed Launcher updates applied after approval.

During play, `F10` opens the stop confirmation, `ESC` cancels it, and `F11` switches the display mode. See [FEATURES](../docs/FEATURES.md) for controls and limitations. An implemented feature does not guarantee compatibility with every PC or online service.

## Requirements

| Component | Requirement |
|---|---|
| OS | Windows 10 / 11 x64 |
| CPU / RAM | x64 CPU. Minimum CPU, instruction-set and memory requirements are not yet established |
| GPU | Vulkan- or OpenGL-capable GPU with suitable drivers; Vulkan is preferred |
| Runtime | Microsoft Edge WebView2 Evergreen Runtime. Setup stops if it is missing and does not install it automatically |
| Storage | Space for the application, game, MLC, saves, packs, caches and installation/update work; no fixed minimum yet |

Standard support for Windows 10 **ended on 2025-10-14**. Listing it as a target OS does not guarantee OS security support. See [Microsoft's notice](https://www.microsoft.com/en-us/windows/end-of-support).

Acceptance so far has mainly used one Windows 11 development host, an isolated, owned JPN v560 game and a limited GPU selection. Full acceptance across Windows 10, clean PCs, all regions, input/audio devices and extended sessions is incomplete. The Core-less Standalone development build cannot run the game.

## Installation

1. Read [INSTALLATION](../docs/INSTALLATION.md) for requirements and safety checks.
2. Obtain Setup from the official [GitHub Releases](https://github.com/IGNSeed/MCWiiU-Launcher/releases/tag/v0.4.3) and choose an installation directory. Setup **does not use Authenticode signing**. Follow the guide's independent source and integrity checks.
3. Start `MCWiiU-Launcher.exe` (Bootstrapper), install your own game data, and select `PLAY` on Home. The application is `MCWiiU Launcher.exe`; the updater is `MCWiiU-Updater.exe`.

Automatic migration from older development builds is not officially supported. The installation guide also covers uninstalling and data retention.

## Game data

**The game, Update, DLC, saves, Wii U system files and game keys are not included.** You must supply your own lawfully obtained data.

Use Update **v560** from the same region as BASE. DLC is optional. [GAME_DATA](../docs/GAME_DATA.md) explains installation, region changes, removal of game data only, and save retention. Back up valuable saves before making changes.

## Updates

Normal Launcher updates apply a signed ZIP after user approval. Verification covers Ed25519, SHA-256, GitHub Asset binding and the file inventory. If the 30-second Readiness check fails, Rollback returns to a verifiable previous version. The user confirmed a real v0.4.1 → v0.4.2 update. For v0.4.2 → v0.4.3, production signatures, the actual old parser, settings/World formats and public assets were verified. End-to-end updater application/readiness is **NOT RUN** because no safely isolated Windows profile is available. Foundation 1.0.2 / Protocol 2 remain; no manual Foundation update is required.

Foundation uses a **manual Setup update**, separate from normal Launcher updates. See [UPDATING](../docs/UPDATING.md) for details and recovery.

## Documentation

The detailed guides below are currently in Japanese.

- [Install and uninstall](../docs/INSTALLATION.md)
- [Owned game data and saves](../docs/GAME_DATA.md)
- [Controls and settings](../docs/FEATURES.md)
- [Updates and recovery](../docs/UPDATING.md)
- [Troubleshooting](../docs/TROUBLESHOOTING.md)
- [Stored data and connections](../docs/PRIVACY.md)
- [Licenses and corresponding source](../docs/THIRD_PARTY_NOTICES.md)
- [Distribution checks and security reports](../SECURITY.md)
- [Changelog](../CHANGELOG.md)
- Development, builds and contributions: [BUILD](../BUILD.md) · [CONTRIBUTING](../CONTRIBUTING.md)

GitHub Issues is the planned bug-report channel, but **it is currently disabled and reporting arrangements are incomplete**. Never include game keys, tokens, account information or saves in public reports.

## Credits

Thanks to [Cemu](https://github.com/cemu-project/Cemu) and its contributors, and the developers of saucer, WebView2, Discord Social SDK and the other dependencies. See [THIRD_PARTY_NOTICES](../docs/THIRD_PARTY_NOTICES.md) for original notices and corresponding source.

## License

The **MPL-2.0** license and copyright notices for Cemu-derived code are retained. See the original [LICENSE.txt](../LICENSE.txt). Third-party dependencies have their own terms. The source license does not grant permission to redistribute game assets or third-party images.

## Disclaimer

MCWiiU Launcher is not an official product of Cemu, Mojang, Microsoft or Nintendo, and is not provided or endorsed by them. Minecraft and related names and assets belong to their respective rights holders.
