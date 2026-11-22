# インストール

[README](../README.md) / [ゲームデータ導入](GAME_DATA.md)

**2026-11-23 時点で一般公開 Setup はありません。** 以下は実装に沿った公開後の導入手順です。本番鍵・配布物の最終受入は未完了です。

## 事前確認

- [必要環境](../README.md#必要環境と確認範囲)を確認します。対象は Windows 10 / 11 x64 です。
- [Microsoft Edge WebView2 Evergreen Runtime](https://developer.microsoft.com/en-us/microsoft-edge/webview2/)を確認します。Edge ブラウザーの存在だけでは Runtime 導入済みと断定できません。Setup は Runtime が見つからないと停止し、ダウンロード・導入を代行しません。
- 所有する Minecraft: Wii U Edition の展開済み BASE と、同じ地域の Update v560 を用意します。[対応形式](GAME_DATA.md)を参照してください。
- 既存環境を使っていた場合は、ゲームと Launcher を正常終了し、ユーザーデータを別媒体へバックアップしてください。旧開発版・旧 MSI からの自動移行は正式サポート外です。

## Setup の取得と導入

1. [正規 GitHub Releases](https://github.com/IGNSeed/MCWiiU-Launcher/releases)を開き、所有者が `IGNSeed`、リポジトリが `MCWiiU-Launcher` であることを確認します。広告・第三者ミラーを配布元と取り違えないでください。現在は取得できる正式版がありません。
2. 公開後の Stable Release に添付される `MCWiiU-Launcher-0.4.0-Setup-x64.exe` と、その Release の説明・検証情報を確認します。これは予定ファイル名で、公開済みという意味ではありません。
3. Setup を通常のユーザー権限で起動します。既定の導入先は `%LOCALAPPDATA%\Programs\MCWiiU-Launcher` です。ディレクトリ選択画面で変更できます。同じユーザーだけが書き込める専用フォルダを使用します。
4. Desktop shortcut は任意です。Setup が Runtime 不足・安全でない配置先・未完了更新を報告した場合は、理由を解消してから再試行します。
5. 完了後はスタートメニューの `MCWiiU Launcher`、または導入先の `MCWiiU-Launcher.exe` で起動します。通常は Setup の完了処理からも起動されます。
6. Home が表示されたら `Settings` で入力・描画・音声を確認します。アカウントは `Settings → Account` です。変更は保存処理で永続化されるので、保存中の表示が終わるまで待ちます。
7. Home の `PLAY` から [Minecraft Wii U Setup](GAME_DATA.md#初回導入) を開始します。ゲーム導入完了後は Home へ戻り、改めて `PLAY` を押します。ゲームは自動起動しません。

固定の入口 `MCWiiU-Launcher.exe` は Bootstrapper です。`MCWiiU Launcher.exe` は版別フォルダ内の本体、`MCWiiU-Updater.exe` は更新専用です。通常の起動では Bootstrapper を使用し、版別 EXE や更新ファイルを手作業で差し替えないでください。

## Windows の警告と配布確認

Setup は **Authenticode 署名を使用しません**。Windows が発行元を認証した Setup として案内していません。SmartScreen の警告や Smart App Control のブロックが発生する可能性があります。

警告が出たら、配布元・Release の状態・ファイル名・配布者が示す独立した検証情報を再確認してください。判断できなければ実行を中止します。Windows Defender、SmartScreen、Smart App Control を無効化する手順は案内しません。未署名 Setup の独立した信頼確認手順は一般配布前の残課題です。

Release に SHA-256 が掲載された場合は、PowerShell の `Get-FileHash -LiteralPath '.\MCWiiU-Launcher-0.4.0-Setup-x64.exe' -Algorithm SHA256` で一致を確認できます。**同じ配布場所にあるハッシュ値との一致だけでは、発行元の真正性までは保証できません。** 内部の Ed25519 検証と Windows の発行元認証も別の仕組みです。

## 保存先とアンインストール

プログラムの導入先と、ユーザーデータの `%LOCALAPPDATA%\MCWiiU-Launcher` は別です。後者の `Data` にゲーム・MLC・セーブ・設定などを保持します。[保存データ一覧](PRIVACY.md#ローカル保存)を参照してください。

通常の削除は、Launcher とゲームを正常終了してから Windows の「設定 → アプリ」で `MCWiiU Launcher` を選び、アンインストールします。Windows 10 では「アプリと機能」、Windows 11 では「インストールされているアプリ」が入口です。

実装は検証できた版別プログラムと登録・ショートカットを削除し、ユーザーデータを保持する設計です。ゲーム・セーブ・Account・Mii・パック・設定は通常のアンインストール後も残ります。未知・不正なファイルや未完了の更新がある場合は、安全のため停止したりファイルを残したりします。すべてのファイルの完全削除を保証するものではありません。

完全削除が必要でも、まずユーザーデータをバックアップし、必要なセーブとアカウントが退避できたことを確認してください。本ガイドには一括削除コマンドを掲載しません。旧 Cemu 環境や別の MLC を Launcher の残骸として削除しないでください。失敗時は[TROUBLESHOOTING](TROUBLESHOOTING.md)を参照します。
