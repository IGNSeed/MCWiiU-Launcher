# MCWiiU Launcher

Minecraft: Wii U Edition v560 を Windows PC で遊ぶための、Cemu を基盤とした非公式ランチャーです。ゲームの導入から、アカウント・ワールド・パックの管理、起動、更新までを一つの画面にまとめます。

**初回リリース準備中です。** 2026-11-23 時点で正式な GitHub Release と一般公開 Setup はありません。初回配布は **0.4.0 Stable** を予定しています。Foundation は **1.0.2**、Updater Protocol は **2**。本番 Ed25519 公開鍵も未設定のため、現在の開発版で本番自動更新を利用できる状態ではありません。

MCWiiU Launcher is an unofficial Windows x64 launcher for Minecraft: Wii U Edition v560, built on Cemu. The first stable release is being prepared; no public installer is available yet. Users must supply their own lawfully obtained game data.

## はじめに

1. [インストールガイド](docs/INSTALLATION.md)で環境と配布元を確認します。公開前のため、現時点では Setup を取得できません。
2. 公開後に Setup を導入し、[ゲームデータ導入ガイド](docs/GAME_DATA.md)に沿って所有する BASE と同じ地域の Update v560 を取り込みます。DLC は任意です。
3. Home の `PLAY` で起動します。終了・キー操作・設定は[機能ガイド](docs/FEATURES.md)を参照してください。

ゲーム本体・Update・DLC・セーブ・Wii U システムファイル・ゲーム鍵は同梱しません。ユーザー自身で適法に用意してください。旧開発版からの自動移行は正式サポート外です。

## 主な機能

| 機能 | 現在の内容 |
|---|---|
| Home / Minecraft 起動 | Native 版の `PLAY`、同じウィンドウでのプレイ、F10 終了確認、F11 表示切替 |
| Game Data | Settings 内で BASE・Update v560・DLC の導入、再インストール、地域変更、ゲームデータのみの削除 |
| Account / Friends | Settings 内のアカウント管理とサービス選択、Friends の一覧・申請管理。通信先の利用条件と接続状態に依存 |
| World | ワールドのインポート、名前・アイコン変更、Account 間移動、Trash と復元 |
| Texture / Graphic Packs | ローカルパックの検証・選択・設定。次回ゲーム起動へ反映 |
| Session History | 日・週・月・年ごとのプレイ時間と日付別履歴 |
| Discord Rich Presence | 閲覧ページ・プレイ状態の通知。Discord 側の共有設定に依存 |
| 更新 / Settings | 承認後に適用する署名付き Launcher 更新の実装、Foundation 手動 Setup 案内、入力・描画・音声・Overlay 設定 |

操作と制約は[FEATURES](docs/FEATURES.md)、更新の準備状況は[UPDATING](docs/UPDATING.md)を参照してください。実装の存在は、すべての PC・オンラインサービスでの動作保証を意味しません。

## 必要環境と確認範囲

| 項目 | 対象・条件 |
|---|---|
| OS | Windows 10 / 11 x64。Windows 向け MSVC x64 製品。ARM64 ネイティブ版・他 OS 版は対象外 |
| CPU | x64 CPU。必要な命令セットを含む最低 CPU 型番・性能基準は未確定 |
| GPU | Vulkan または OpenGL に対応した GPU と適切なドライバー。Vulkan を優先。全メーカー・全機種の受入は未完了 |
| RAM | 最低容量・推奨容量は未確定。エミュレーションと WebView2 の動作に必要な余裕が必要 |
| Runtime | Microsoft Edge WebView2 Evergreen Runtime。Setup は存在を確認し、未導入なら停止。Runtime の自動導入はしません |
| ストレージ | 本体に加え、所有ゲーム・MLC・セーブ・パック・キャッシュと導入/更新作業用の空き容量が必要。固定の最低容量は未確定 |

Windows 10 の通常サポートは **2025-10-14 に終了**しています。Launcher の対象 OS という表記は OS のセキュリティサポートを保証しません。Microsoft の[終了案内](https://www.microsoft.com/en-us/windows/end-of-support)を確認してください。

既存受入は主に同一開発ホストの Windows 11、所有する隔離 JPN v560、限定 GPU で行われました。Phase 13 では描画・F10/ESC/F11・正常停止・隔離更新/Rollback を確認済みです。Windows 10 の実動作、クリーン PC、すべての地域・入力・音声・長時間動作は一括して検証済みとしていません。

## ダウンロードと案内

正規の配布先は [IGNSeed/MCWiiU-Launcher の GitHub Releases](https://github.com/IGNSeed/MCWiiU-Launcher/releases)です。**現在は初回リリース準備中**です。Setup は Authenticode 署名を使用しない方針です。[導入時の安全確認](docs/INSTALLATION.md)を参照してください。

| 知りたいこと | ガイド |
|---|---|
| 導入・アンインストール | [INSTALLATION](docs/INSTALLATION.md) |
| BASE / Update / DLC・セーブ保持 | [GAME_DATA](docs/GAME_DATA.md) |
| 画面の操作・設定・F10 / F11 | [FEATURES](docs/FEATURES.md) |
| Launcher 更新・Foundation 手動更新・復旧 | [UPDATING](docs/UPDATING.md) |
| 起動・入力・音声などの問題 | [TROUBLESHOOTING](docs/TROUBLESHOOTING.md) |
| 保存データと外部通信 | [PRIVACY](docs/PRIVACY.md) |
| 配布確認とセキュリティ報告 | [SECURITY](SECURITY.md) |
| ライセンス・原文通知・対応ソース | [THIRD_PARTY_NOTICES](docs/THIRD_PARTY_NOTICES.md)・[LICENSE](LICENSE.txt) |
| 変更履歴 | [CHANGELOG](CHANGELOG.md) |
| 開発・ビルド・貢献 | [BUILD](BUILD.md)・[CONTRIBUTING](CONTRIBUTING.md) |

不具合の報告先は、このリポジトリの [GitHub Issues](https://github.com/IGNSeed/MCWiiU-Launcher/issues)を予定しています。**現在は Issues が無効で、報告受付の整備は未完了です。** 投稿可能になった際も[報告情報の注意](docs/TROUBLESHOOTING.md)を守り、機密情報は公開しないでください。

## ライセンスとクレジット

[Cemu](https://github.com/cemu-project/Cemu) とその貢献者に感謝します。Cemu 由来コードの MPL-2.0 と著作権表示を維持します。本文は [LICENSE.txt](LICENSE.txt)、依存物は[第三者通知](docs/THIRD_PARTY_NOTICES.md)を参照してください。

MCWiiU Launcher は Cemu の公式製品ではありません。Mojang、Microsoft、Nintendo から提供・承認された製品でもありません。Minecraft および関連名称・素材の権利は各権利者に帰属します。ソースのライセンスは、ゲーム資産や第三者素材の再配布許諾を与えるものではありません。
