# MCWiiU Launcher 変更履歴

以下は Launcher 固有の変更履歴で、上流 Cemu のリリース履歴ではありません。

## [0.4.2 Stable](https://github.com/IGNSeed/MCWiiU-Launcher/releases/tag/v0.4.2) — 2026-11-23

Launcher 0.4.2 / Foundation 1.0.2 / Updater Protocol 2。[Setup](https://github.com/IGNSeed/MCWiiU-Launcher/releases/download/v0.4.2/MCWiiU-Launcher-0.4.2-Setup-x64.exe) / [ZIP](https://github.com/IGNSeed/MCWiiU-Launcher/releases/download/v0.4.2/MCWiiU-Launcher-0.4.2-win-x64.zip)。配布ソースCommit: [c758cb39180ff28311638315ffa90b80994b0bca](https://github.com/IGNSeed/MCWiiU-Launcher/commit/c758cb39180ff28311638315ffa90b80994b0bca)。TagはこのCommitに固定しています。

- JPN v560の初回Mii DB自動初期化。一度のPLAYで専用セッション、検証、通常のMinecraft起動へ進みます。正常なHDB / ODBがあれば直接起動します。
- 検証済みFFL完了境界でCoreを正常停止し、既知の初回クラッシュ経路への進行を防ぎます。RPX・guest戻り値の書換えや意図的なクラッシュは行いません。
- 3DBのヘッダー・完全長・CRCを検証し、不足分だけを上書きなしで追加します。既存の破損DBは保持してエラーにします。
- 完了した作業MLCは、セッション所有情報・実ファイルID・内容・排他handleを確認して整理します。復旧を先に処理し、中断したCleanupは再開可能です。使用中・権限不足・未知ファイル・所有不明の旧作業データは保持します。
- 既存journal v1とFoundation 1.0.2 / Protocol 2を維持。v0.4.1の実設定パーサー・署名検証・公開Asset照合との互換性を確認しました。

検証済みRPX identityと所有するFFLリソースが必要です。USA / EURのMii自動初期化は未対応。隔離環境でOpenGL / Vulkanの初回・2回目メニュー、正常停止、約1.53GBの作業MLC整理、Account・セーブ・原本の保持を確認しました。本番署名・Setup bundle・4ファイルの匿名ダウンロードも確認済みです。実ユーザープロファイルでのSetup実行・実自動更新全工程・全ゲーム回帰・他GPU / OSはNOT RUN。既知の素材許諾・ライセンス/対応ソース・依存評価の未確認事項は継続します。

## [0.4.1 Stable](https://github.com/IGNSeed/MCWiiU-Launcher/releases/tag/v0.4.1) — 2026-11-23

Launcher 0.4.1 / Foundation 1.0.2 / Updater Protocol 2。[Setup](https://github.com/IGNSeed/MCWiiU-Launcher/releases/download/v0.4.1/MCWiiU-Launcher-0.4.1-Setup-x64.exe) / [ZIP](https://github.com/IGNSeed/MCWiiU-Launcher/releases/download/v0.4.1/MCWiiU-Launcher-0.4.1-win-x64.zip)。配布ソース Commit: [6b074e5a329f64fb26fca7e370c83d318e7f9b39](https://github.com/IGNSeed/MCWiiU-Launcher/commit/6b074e5a329f64fb26fca7e370c83d318e7f9b39)。Tag はこの Commit に固定しています。

- Add Account はインポート成功後に閉じ、失敗・キャンセル時は保持します。
- 8言語＋System Default、言語の保存と再起動復元。Home / Friends / World などのタブ名は英語固定です。
- 重複する小さい UI 説明文を整理しました。
- 新規 TV / GamePad Audio Output は Default。既存 Disabled・個別デバイス指定は保持します。
- 共有設定を旧版互換 v2 とし、言語を独立保存。v0.4.0 からの読込みと、旧版へ戻した場合の更新設定の読書きを確認しました。不正・将来版の既存設定は上書きしません。
- Setup の製品表示・ファイル版を Launcher 版へ統一しました。Foundation 版は 1.0.2 を維持します。

本番 Ed25519 署名、v0.4.0 実パーサーによる Manifest 受理と公開 Asset 照合、Setup bundle、4ファイルの匿名ダウンロードとハッシュを確認しました。実自動更新・実ユーザー環境へのインストール・ゲーム全面回帰は未実施です。既知の素材許諾・ライセンス/対応ソース・依存評価等の未確認事項は引き続き未完了です。

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
