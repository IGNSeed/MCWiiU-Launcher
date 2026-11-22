<p align="center">
  <img src="src/launcher/assets/app-icon.png" alt="MCWiiU Launcher" width="112">
</p>

<h1 align="center">MCWiiU Launcher</h1>

<p align="center">Minecraft: Wii U Edition を、ひとつのランチャーから。</p>

<p align="center">
  <strong>0.4.0 Stable — 公開準備中</strong><br>
  Windows 10 / 11 x64 · Cemu-based · Unofficial
</p>

<p align="center">
  <strong>日本語</strong> · <a href="i18n/README.en.md">English</a> · <a href="i18n/README.ko.md">한국어</a> · <a href="i18n/README.zh-CN.md">简体中文</a> · <a href="i18n/README.es.md">Español</a>
</p>

<p align="center">
  <a href="docs/INSTALLATION.md">インストールガイド</a> · <a href="docs/FEATURES.md">機能ガイド</a>
</p>

---

## 概要

MCWiiU Launcher は、Minecraft: Wii U Edition **v560** を Windows PC で遊ぶための、Cemu を基盤とした非公式ランチャーです。ゲームの導入から、アカウント・ワールド・パックの管理、起動、更新までを一つの画面にまとめます。

## リリース準備状況

**初回リリース準備中です。現在、正式な GitHub Release と一般公開 Setup はありません。** 初回配布は **0.4.0 Stable**、Foundation は **1.0.2**、Updater Protocol は **2** を予定しています。

本番 Ed25519 公開鍵は未設定です。画像素材の再配布許諾、ライセンス・対応ソースの最終照合、依存ライブラリの残存脆弱性、クリーンビルドと配布試験、クリーン Windows 10 / 11 での受入が配布前の確認事項です。現時点の開発成果物は一般配布用ではありません。

## 主な機能

- **Minecraft Launch / Game Data** — Native 版の `PLAY` から同じウィンドウでプレイ。BASE・同じ地域の Update v560・任意の DLC を導入・管理します。
- **Account / Friends** — Settings のアカウント管理とサービス選択、Friends の一覧・申請管理。利用条件と接続状態に依存します。
- **World Manager** — ワールドのインポート、名前・アイコン変更、Account 間移動、Trash と復元。
- **Texture Pack / Graphic Pack Manager** — ローカルパックの検証・選択・設定。次回ゲーム起動に反映します。
- **Session History / Discord Rich Presence** — 日・週・月・年のプレイ時間と日付別履歴、閲覧ページ・プレイ状態の通知。Presence は Discord 側の共有設定にも依存します。
- **Launcher Settings / Secure Updates** — 入力・描画・音声・Overlay の設定と、承認後に適用する署名付き Launcher 更新の実装。

ゲーム中は `F10` で終了確認、`ESC` で取消、`F11` で表示を切り替えます。詳しい操作と制約は [FEATURES](docs/FEATURES.md) を参照してください。実装の存在は、すべての PC・オンラインサービスでの動作保証を意味しません。

## 必要環境

| 項目 | 対象・条件 |
|---|---|
| OS | Windows 10 / 11 x64 |
| CPU / RAM | x64 CPU。最低 CPU・命令セット・メモリ容量の基準は未確定 |
| GPU | Vulkan または OpenGL 対応 GPU と適切なドライバー。Vulkan を優先 |
| Runtime | Microsoft Edge WebView2 Evergreen Runtime。Setup は未導入なら停止し、自動導入しません |
| ストレージ | 本体・ゲーム・MLC・セーブ・パック・キャッシュと導入/更新用の空き容量。最低容量は未確定 |

Windows 10 の通常サポートは **2025-10-14 に終了**しています。対象 OS の表記は OS のセキュリティサポートを保証しません。[Microsoft の案内](https://www.microsoft.com/en-us/windows/end-of-support)をご確認ください。

これまでの受入は主に同一開発ホストの Windows 11、所有する隔離 JPN v560、限定 GPU で実施しています。Windows 10、クリーン PC、全地域・入力・音声・長時間動作の全面受入は未完了です。Core のない Standalone 開発版はゲームを起動できません。

## インストール

1. [INSTALLATION](docs/INSTALLATION.md) で必要環境と安全確認を読みます。**現在、ダウンロードできる正式 Setup はありません。**
2. 公開後は、正規の [GitHub Releases](https://github.com/IGNSeed/MCWiiU-Launcher/releases) から Setup を取得し、導入先を選びます。Setup は **Authenticode 署名を使用しません**。ガイドの独立した配布元・整合性の確認を行ってください。
3. `MCWiiU-Launcher.exe`（Bootstrapper）から起動し、所有ゲームを導入して Home の `PLAY` を選びます。本体名は `MCWiiU Launcher.exe`、Updater は `MCWiiU-Updater.exe` です。

旧開発版からの自動移行は正式サポート対象外です。アンインストールとデータ保持についても導入ガイドを参照してください。

## ゲームデータ

**ゲーム本体・Update・DLC・セーブ・Wii U システムファイル・ゲーム鍵は同梱しません。** ご自身が所有する、適法に用意したデータが必要です。

BASE と同じ地域の Update **v560** を使用してください。DLC は任意です。導入・地域変更・ゲームデータのみの削除とセーブ保持は [GAME_DATA](docs/GAME_DATA.md) で説明しています。作業前に大切なセーブをバックアップしてください。

## 更新

通常の Launcher 更新は、利用者の承認後に署名付き ZIP を適用する方式です。Ed25519、SHA-256、GitHub Asset の対応、ファイル一覧を検証し、30秒の Readiness 確認に失敗した場合は検証可能な以前の版へ Rollback します。**本番鍵が未導入のため、現在の開発版で本番自動更新は利用できません。**

Foundation は **手動 Setup 更新**です。通常の Launcher 更新とは別の工程になります。詳細と復旧方法は [UPDATING](docs/UPDATING.md) を参照してください。

## ドキュメント

- [導入・アンインストール](docs/INSTALLATION.md)
- [所有ゲームとセーブ](docs/GAME_DATA.md)
- [操作・設定](docs/FEATURES.md)
- [更新・復旧](docs/UPDATING.md)
- [問題の対処](docs/TROUBLESHOOTING.md)
- [保存データと通信](docs/PRIVACY.md)
- [ライセンス・対応ソース](docs/THIRD_PARTY_NOTICES.md)
- [配布確認・セキュリティ報告](SECURITY.md)
- [変更履歴](CHANGELOG.md)
- 開発・ビルド・貢献: [BUILD](BUILD.md) · [CONTRIBUTING](CONTRIBUTING.md)

不具合の報告先には GitHub Issues を予定していますが、**現在は無効で、報告受付の整備は未完了です**。公開の報告にゲーム鍵・トークン・アカウント情報・セーブを含めないでください。

## クレジット

[Cemu](https://github.com/cemu-project/Cemu) とその貢献者、saucer、WebView2、Discord Social SDK および各依存ライブラリの開発者に感謝します。原文通知と対応ソースについては [THIRD_PARTY_NOTICES](docs/THIRD_PARTY_NOTICES.md) を参照してください。

## ライセンス

Cemu 由来コードの **MPL-2.0** と著作権表示を維持します。原文は [LICENSE.txt](LICENSE.txt) です。第三者依存物にはそれぞれの条件が適用されます。ソースのライセンスは、ゲーム資産や第三者画像の再配布許諾を与えるものではありません。

## 免責事項

MCWiiU Launcher は Cemu、Mojang、Microsoft、Nintendo の公式製品ではなく、各社から提供・承認されたものでもありません。Minecraft および関連名称・素材の権利は各権利者に帰属します。
