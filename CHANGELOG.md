# MCWiiU Launcher 変更履歴

以下は Launcher 固有の変更履歴で、上流 Cemu のリリース履歴ではありません。

## [0.4.0 Stable](https://github.com/IGNSeed/MCWiiU-Launcher/releases/tag/v0.4.0) — 2026-11-23

初回 Stable Release。Launcher Version 0.4.0、Foundation Version 1.0.2、Updater Protocol 2。[Setup](https://github.com/IGNSeed/MCWiiU-Launcher/releases/download/v0.4.0/MCWiiU-Launcher-0.4.0-Setup-x64.exe) を公開し、本番 Ed25519 署名・配布ファイルの整合性・匿名ダウンロードを確認済みです。

配布バイナリのソース Commit: [52d4fbbaddc9edac0eb7d0eb6242ca91a8bc8b7c](https://github.com/IGNSeed/MCWiiU-Launcher/commit/52d4fbbaddc9edac0eb7d0eb6242ca91a8bc8b7c)。Tag はこの Commit に固定しています。

- Windows x64 の saucer / WebView2 UI と同一ウィンドウの Native Cemu 描画。Minecraft v560 の PLAY、正常停止、F10 終了確認、F11 全画面切替。
- Phase 1〜4 の起動基盤・LocalAppData 管理・初回セットアップ・Game Data 管理、JPN/USA/EUR のタイトル認識と同地域 Update v560 の検証。
- Account とサービス設定、Friends 一覧・申請管理。全オンラインサービスの受入は未完了。
- World のインポート、名前/アイコン変更、Account 間移動、Trash・復元・明示的な完全削除。Texture / Graphic Packs の検証・選択・次回起動反映。
- 入力・描画・音声・Overlay 設定、管理画面の外部変更追従、Session History の期間別プレイ時間表示（Phase 5〜9 を含む継続開発）。
- Inno Setup、Bootstrapper、独立 Updater、署名付き版別更新、Readiness と Rollback（Phase 10）。
- Discord Social SDK の Rich Presence と再接続処理・対象範囲の受入（Phase 11）。公開プロフィール表示等は未完了。
- Launcher 本体の正式名を `MCWiiU Launcher.exe`、固定入口を `MCWiiU-Launcher.exe`、Updater を `MCWiiU-Updater.exe` とする構成、Foundation 1.0.2（Phase 12）。
- Launcher 0.4.0、通常 ZIP 更新の維持と Foundation 手動 Setup 案内、署名ツール、Asset binding、125原文通知の採取、依存物の限定更新、旧 Cemu 公開 workflow の無効化（Phase 13、開発 Commit `d073f23d1f098cf6eb70e9f96a6285267d37ac48`）。
- 日本語の導入・機能・更新・復旧・プライバシー・開発ガイドを整備（Phase 14）。製品コードと既存ユーザーデータは変更しません。

素材許諾、ライセンス・対応ソースの未確認事項、全依存/CVE 評価、クリーン PC と追加 OS/GPU/入力/音声の全面受入、次版への実更新試験は引き続き未完了です。[更新の現状](docs/UPDATING.md)と[第三者通知](docs/THIRD_PARTY_NOTICES.md)を参照してください。過去の開発版を正式な配布履歴や移行保証として扱いません。
