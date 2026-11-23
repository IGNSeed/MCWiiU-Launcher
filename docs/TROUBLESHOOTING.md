# トラブルシューティング

[README](../README.md) / [更新と復旧](UPDATING.md)

原因欄は確認候補です。エラー表示だけで原因を断定しません。ゲームと Launcher を通常の操作で終了し、元データを保全してから対処してください。Windows の保護機能を無効化する、システムファイルを書き換える、セーブや MLC を削除する対処は案内しません。

## 導入・起動

| 症状・表示 | 考えられる原因と確認 | 安全な対処 |
|---|---|---|
| Launcher が起動しない | 誤った EXE、Runtime 不足、破損した版、Windows によるブロック。配布元・実行した EXE・警告を確認 | スタートメニューか `MCWiiU-Launcher.exe` から起動。[導入ガイド](INSTALLATION.md)の Runtime・署名方針を確認。[v0.4.2 Stable](https://github.com/IGNSeed/MCWiiU-Launcher/releases/tag/v0.4.2)を取得できます |
| `Install the Microsoft Edge WebView2 Evergreen Runtime before installing the launcher.` | Setup が Runtime の登録を確認できない | Microsoft 公式の Runtime 導入を確認して再試行。Edge 本体の有無とは区別 |
| `Installation stopped. Close the launcher normally...` | 稼働中、未完了更新、安全でない配置先など | 通常終了後、専用導入先・未完了状態を確認。ジャーナルを消して強行しない |
| `The signed program bundle could not be verified. Nothing will be installed.` | 改変・破損・誤った署名/入力など | 実行を止め、配布元・ファイルを確認。本番署名の検証失敗を迂回しない |
| `Launcher readiness failed. Use a verified Setup for manual recovery.` | 起動後の準備確認が失敗 | データを保持し、[Rollback・手動復旧](UPDATING.md)を参照 |
| アンインストールできない | 稼働中・未完了更新・未知のファイル・検証器破損 | 表示を保存して通常終了。ユーザーデータを保全し、検証済み Setup での修復を検討。フォルダ一括削除はしない |

## Minecraft・Game Data・Account

| 症状・表示 | 考えられる原因と確認 | 安全な対処 |
|---|---|---|
| PLAY が無効 / ゲームが起動しない | Core 未準備、Standalone、導入不足、別操作・保存中 | Home の理由と `Settings → Game Data` の状態を確認。Native 版、BASE + 同地域 Update v560 が必要 |
| Mii初期化エラー / `This JPN RPX is not supported...` | 未対応identity、FFLリソース・Account不足、既存DB破損、容量不足等 | [Mii初期化の対応条件](GAME_DATA.md#初回miiデータベースの自動初期化)を確認。USA / EURの自動初期化は未対応。ダミーDBや別環境のファイルで検査を通さない |
| `Mii data initialization could not stop. Restart Launcher.` | 専用セッションの安全な停止を確認できない | 再初期化や通常PLAYを強行せず、表示どおりLauncherの再起動が必要。既存DB・作業記録を保全 |
| `.mii-initialization`の作業コピーが残る | 使用中、権限、未知ファイル、未完了復旧、旧形式で所有不明等 | 正常終了後の安全な再試行に任せる。所有不明の旧コピーは自動削除されない。MLC・journalの一括削除で解消しない |
| PLAY 後クラッシュ・黒画面 | データ不足、外部パック、描画 API / GPU / ドライバー等。原因未特定の場合もある | エラーと起動条件を記録。停止後に有効パックを確認し、必要なら選択を戻す。管理データを消さない |
| 「タイトルフォルダまたはmeta/meta.xmlを選択してください。」 | 選択階層・形式が違う | [必須構造](GAME_DATA.md#対応形式)を確認し、元のタイトルフォルダまたは `meta/meta.xml` を選択 |
| 「メタデータ、タイトルID、または選択セッションを確認してください。」 | XML 不整合、選択後の原本変更など | 所有原本を確認して選び直す。XML 改変で検査を通さない |
| 「Updateはv560のみ利用できます。」 / 「BASEとUpdateの地域を一致させてください。」 | Update 版または地域が不一致 | v560 と BASE の地域を照合。DLC の他地域互換性は保証外 |
| 「既にインストールされています。再インストール操作を使用してください。」 | 管理済みタイトルを通常導入で指定 | `Reinstall BASE` / `Reinstall Update` を使用し、対象を確認 |
| 「空き容量が不足しています。」 / 「アクセスが拒否されました。」 | 作業容量・権限・使用中ファイル | 所有原本・セーブを保持し、容量と同時利用を確認。不要な他ファイルの整理も所有者判断で行う |
| 「復旧が必要です。」 / `RecoveryRequired` | 操作記録とファイルの照合が必要 | データと操作記録を残して終了。時刻・操作・対象種別を記録し、復旧を相談。ジャーナル/MLC/セーブを消さない |
| Account Not Ready / Account 関連エラー | Active Account、必要ファイル、サービス設定の不足 | `Settings → Account` で状態を確認。最初は Offline を使用できるか確認。自分の認証情報だけを使用し、Account や鍵を公開しない |

## 描画・入力・音声・周辺機能

| 症状 | 考えられる原因と確認 | 安全な対処 |
|---|---|---|
| Vulkan / OpenGL で表示に問題 | GPU・API・ドライバー・パック条件の不一致等 | 停止後 `Settings → Graphics` の Graphics API / GPU とパック条件を確認。切替可能な API を試す場合は元設定を控える。全 GPU の保証は未完了 |
| コントローラーが反応しない | 選択違い、割当、デッドゾーン、切断、フォーカス | Input の Physical Controller / Profile / Controller Input Test を確認。再接続後は対象を確認し、ゲームへフォーカス。Input Test の成功後もゲーム内動作を確認 |
| 音が出ない・入力音声がない | API・出力先・音量・Windows 側設定、ゲームの入力要求 | Audio の TV / GamePad Output、Volume、Audio API と Windows 音量を確認。Microphone は要求時に使用。メーターだけで人間に聞こえることを保証しない |
| Texture / Graphic Packs を変更しても反映しない | ゲーム実行中、未適用、検証エラー、対象外パック | 検証表示を確認し、ゲーム停止後に選択・適用して次回 PLAY。外部変更の一覧更新は自動適用と別 |
| World の復旧表示 | 中断した変更または完全削除がある | 表示する復旧内容を読む。完全削除の続行と変更前への復旧を取り違えず、バックアップを保持 |
| Session History の `!` / 保存警告 | 破損、未知の新しい形式、ロック、保存失敗等 | 履歴とバックアップを残し、警告文を記録。空ファイルや既定値で上書きしない |
| Discord Rich Presence が見えない | Discord 未起動、SDK/RPC 接続、アクティビティ共有設定 | Discord の状態と自身の共有設定を確認。Launcher は Discord 不在でも使用可能。共有設定を変更するかは利用者が判断。成功応答と実際の公開表示は別 |
| 更新できない・Foundation が必要・Rollback | 未公開 Release、本番鍵未設定、接続、署名、基盤不足、起動準備失敗 | [UPDATING](UPDATING.md)で該当状態を確認。署名エラーを回避したり Manifest を改変したりしない |

## 解決しない場合の報告

[このリポジトリの Issues](https://github.com/IGNSeed/MCWiiU-Launcher/issues)は **現在無効**です。公開報告先の有効化は準備事項です。上流 Cemu の Discord・Wiki はこの Launcher 専用窓口ではありません。利用可能になった際は、次の情報を必要最小限で添えてください。

- Launcher / Foundation の版、正式 Setup か開発ビルドか、実行した EXE。
- Windows の版・ビルド、GPU・ドライバー、Vulkan / OpenGL、入力/音声デバイスの種類。
- 再現手順、期待結果と実際の結果、発生時刻、正確なエラーメッセージ。
- ゲームの地域と Update v560、関係するパックの名前・有効状態。
- `%LOCALAPPDATA%\MCWiiU-Launcher\Logs` 等の該当部分を、自身で内容を確認・伏字化した抜粋。

ユーザー名を含むパス、メール、PNID・Account ID、IP、認証 Cookie/Token、秘密鍵、ゲーム鍵、Account 原本、MLC・セーブ全体、ゲームファイルは添付しないでください。ログは完全に匿名化されているとは保証していません。伏字化できない場合は公開添付を止め、セキュリティ問題は[SECURITY](../SECURITY.md)を確認してください。
