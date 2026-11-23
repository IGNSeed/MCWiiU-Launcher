# 更新と復旧

[README](../README.md) / [インストール](INSTALLATION.md)

**[0.4.2 Stable](https://github.com/IGNSeed/MCWiiU-Launcher/releases/tag/v0.4.2) を公開し、本番署名・4ファイルの匿名ダウンロード・v0.4.1の実パーサーによるManifestと設定の互換性を確認済みです。** 実際の自動更新全工程はNOT RUNです。安全にKnown FolderとHKCUを隔離できるWindowsプロファイルがないため、正式インストール先での適用試験は行っていません。

## v0.4.1からv0.4.2への互換性

Foundation **1.0.2** / Updater Protocol **2** を維持します。v0.4.1の実コードで新しい本番署名付きManifestと公開ReleaseのAsset情報を照合しました。共有設定v2、独立した言語設定、`Check for Updates on Startup`の真偽値は旧版との読書き・Rollback往復で保持されます。隔離した実ゲーム試験でもAccount・既存セーブ・ゲームのハッシュは不変でした。これは実Updaterでの適用・Readiness・Rollback全工程の成功を意味しません。

Mii初期化の既存journal v1形式は維持し、セッションごとの所有情報を別ファイルへ追加しています。旧Phase 20形式は復旧を先に実行し、所有根拠がない作業コピーを保全します。v0.4.1はこの専用作業領域を処理しません。旧版へ戻しても新しいjournalを削除・書換えする必要はありません。`rollback_safe=true`はこのデータ形式と実パーサーの互換性に基づきます。

配布ソースCommit: `c758cb39180ff28311638315ffa90b80994b0bca`。公開済みTag `v0.4.2`はこのCommitに固定し、後続のドキュメントCommitへ動かしません。

## v0.4.0 との設定互換性（v0.4.1の履歴）

v0.4.1 は共有の `Data/launcher-preferences.json` を v2 のまま保存し、`Check for Updates on Startup` の値を保持します。言語は同じディレクトリの `launcher-preferences.json.language.json`（schema 1）に独立保存します。旧版で設定を保存しても言語ファイルは保持されます。不正な設定、将来版、読み込みエラーのファイルは無条件に上書きしません。

Manifest の `preferences_schema=2` と `rollback_safe=true` は旧版での読み書きを確認した形式に対応します。実際の Updater による適用・Readiness・Rollback 全工程の受入は未実施です。言語設定ファイルや更新記録を手作業で削除・改変して復旧を強行しないでください。

v0.4.1の配布バイナリとSetupのソースCommit: `6b074e5a329f64fb26fca7e370c83d318e7f9b39`。Foundation 1.0.2 / Updater Protocol 2 は維持しています。

## 通常の Launcher 更新

1. `Settings → General → Check for Updates` を押します。`Check for Updates on Startup` が有効な場合は起動時にも確認します。
2. 新しい Stable Release が検出されると通知されます。`View Update` で現在版・対象版・サイズ・変更内容を確認します。
3. `Update Now` でその更新を承認します。`Later` で延期できます。ゲームが動作中なら正常停止と保存完了を待ち、安全な状態で開始します。
4. GitHub の Stable Release と署名済み Manifest を照合し、Ed25519 署名、対象製品・版・Asset ID・URL・互換性を確認します。ZIP を HTTPS で取得し、SHA-256、サイズ、内部ファイルの一覧とパスを検証します。
5. `versions` 以下の新しい版へ準備し、Launcher を終了して独立 Updater へ引き継ぎます。プログラムの通常更新で Setup を自動実行しません。
6. Bootstrapper が新 Launcher を起動し、**30秒の Readiness 確認**を行います。単にプロセスが存在するだけで成功と判断しません。
7. 成功すれば新しい版を確定し、正常版は原則 current と previous の2版を保持します。失敗候補は診断用に残ることがあり、「フォルダは常に2個」とは限りません。

ダウンロード等の中止可能な段階には `Cancel Update` があります。終了・適用中には操作できない段階があります。強制終了や電源断を避けてください。Release が存在せず API が404を返す場合も `You're Up to Date` と表示する実装です。この表示は本番鍵・配信が準備済みという証明ではありません。

## Foundation の手動更新

Foundation は固定入口の `MCWiiU-Launcher.exe`、`MCWiiU-Updater.exe` 等の導入・更新基盤です。現行 Foundation は **1.0.2**、Updater Protocol は **2** です。

通常 ZIP がより新しい Foundation を要求した場合、`FoundationRequired` の案内が出て、ボタンは `Open Official Releases` になります。ここで行うのは[正規 Releases](https://github.com/IGNSeed/MCWiiU-Launcher/releases)を開くことだけです。Foundation の自動ダウンロード・自動 Setup 実行は廃止されています。

利用者が新しい Setup を取得・検証し、ゲームと Launcher を正常終了して、既存の導入先に明示的に実行します。[未署名 Setup の注意](INSTALLATION.md#windows-の警告と配布確認)を参照してください。[0.4.2 Setup](https://github.com/IGNSeed/MCWiiU-Launcher/releases/download/v0.4.2/MCWiiU-Launcher-0.4.2-Setup-x64.exe) を取得できます。

Phase 13 の手動 Foundation 更新は、既存インストールの current / previous Launcher とユーザーデータを保持し、基盤を更新してから通常 ZIP 更新を再開する設計です。対応する隔離試験を実施済みですが、旧開発版すべての移行を保証しません。Setup は稼働中・未完了 pending・不正な既存状態を拒否します。エラーがある場合に JSON を編集して続行しないでください。

## Rollback と安全な復旧

新 Launcher が Readiness を満たさない場合、互換性と署名・ファイル検証を満たす previous があれば旧版へ戻します。隔離試験では約30秒のタイムアウト後の復帰を確認しています。初回導入のように previous がない場合や、旧版も破損している場合は自動復帰できず、検証済み Setup による手動復旧が必要です。

| 状況 | 確認と対処 |
|---|---|
| 更新が見つからない | Stable のみが対象。現在版と Release の版を比較。同じ 0.4.2 への更新は発生しない |
| ネットワークエラー | 接続、GitHub の到達性、プロキシ等を確認して再確認。認証情報をログへ載せない |
| Manifest / 署名 / SHA-256 エラー | その更新を適用せず停止。正規配布元と対象版を確認し、エラーコードを記録。検証を迂回しない |
| Foundation Version 不足 | 上記の手動 Setup 手順へ。版や Manifest の値を書き換えない |
| 更新後に起動しない / Rollback | 固定 Bootstrapper から起動し、復帰結果を確認。繰り返す場合は失敗版と復帰版を記録 |
| `manual-recovery-required` / 未完了更新 | Launcher を終了し、データと更新記録を保全。正規の検証済み Setup と案内で復旧。pending/current/versions を手動削除しない |

手動復旧前は `%LOCALAPPDATA%\MCWiiU-Launcher` の必要データをバックアップします。Launcher の Rollback はプログラムの復帰で、セーブを昔の状態へ巻き戻す機能ではありません。データを伴う互換性は各更新での確認が必要です。旧開発版は正式サポート外で、自動移行を強行せず[導入ガイド](INSTALLATION.md)に従います。
