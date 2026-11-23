# 機能と操作

[README](../README.md) / [問題がある場合](TROUBLESHOOTING.md)

以下は Native 版の現行 UI に沿った案内です。無効な操作はゲーム停止・保存完了・Core 準備を待ちます。Standalone は開発用で、Core を必要とするゲーム・Account・Friends・World・パックなどの操作は利用できません。

## Home・プレイ・終了

ゲームデータ未導入時の `PLAY` は[初回セットアップ](GAME_DATA.md)を開きます。導入済みの BASE + 同地域 Update v560 と Core の準備が整えば `PLAY` でゲームを起動します。起動失敗時は表示理由を確認してください。

JPN v560の対応RPXでは、必要なFFLリソースがあれば初回の`PLAY`で不足Mii DBを自動生成・検証し、作業MLCを整理して通常起動します。USA / EURの自動初期化は未対応です。[対応条件と復旧](GAME_DATA.md#初回miiデータベースの自動初期化)を確認してください。

- **F11**: ゲームのみのボーダーレス全画面と、サイドバー付きウィンドウ表示を往復します。ゲームにフォーカスがある状態で操作します。
- **F10**: ゲームの終了確認を開きます。`ゲームを終了` で正常停止、`キャンセル` または ESC でプレイを続行します。ゲーム内で必要な保存を済ませてください。
- ゲーム中に管理画面を開いた場合は `ゲームに戻る` で戻れます。
- Launcher 右上の閉じる操作は、必要な確認とゲーム・保存・管理処理の正常終了を経て終了します。タスクの強制終了を通常の終了手順にしないでください。

## Settings → Game Data / Account

Game Data は独立したサイドバーページではなく、Settings のタブです。[BASE・Update・DLC の導入と管理](GAME_DATA.md)を参照してください。

Account も Settings のタブです。`Active Account` で利用する Account を選びます。切替・追加・編集・削除はゲーム停止中に行います。

- `Add Account` → `Select ZIP File` で所有するアカウントの ZIP を選択し、候補を確認して `Import Selected Accounts`。`Account Only` と `Account + Online Files` を区別します。後者は自身の適法なオンライン用ファイルが必要で、他人の認証情報を使う操作ではありません。
- `Edit Account` は Mii Name、Birthday、Gender、Country / Region Code、Email 等の表示・編集です。編集可能な項目を変更し、保存の完了を待ちます。実装にない一括保存ボタンはありません。
- `Delete Account` の確認内容を必ず読みます。Game Data の削除と Account の削除は別です。
- `Show Details` の内容には個人情報が含まれます。スクリーンショットや公開 Issue にそのまま載せないでください。
- `Network Service` は `Offline` / `Pretendo` / `Custom`。Custom は自身が管理する `network_services.xml` を選択します。サービス選択や認証ファイルの存在だけで接続成功を保証しません。ゲーム本体の通信と Friends 接続は別で、条件が整えば Friends は PLAY 前にも接続を開始します。

## Friends

`All Friends`、`Online`、`Requests` で一覧を切り替えます。準備できた Pretendo 接続では `Add Friend` に PNID を入力して `Send Request`。受信申請の `Accept` / `Reject`、送信申請の `Cancel Request`、友達のメニューの `Remove Friend` を使用できます。申請・削除などの管理は Pretendo 接続に限定され、Custom の管理操作は受理しません。

Account と選択サービスの準備・接続が必要です。未接続・Offline・準備不足では状態を表示して操作を制限します。Friends 管理の実装は、すべてのサーバーでのオンライン動作や、ゲームへの招待・マルチプレイ成立の保証ではありません。実サービス全経路の受入は未完了です。

## World

Active Account のワールドを表示します。`Open World Folder` のフォルダアイコンで現在の保存先を開けます。

- `Import World`: `World File` の `Browse` でワールド本体を選択。`.ext` の Metadata File は任意です。プレビュー、保存先 Account、名前、必要なアイコンを確認して `Import World`。
- 各ワールドの `Rename` / `Change Icon` / `Move` で名前・アイコン・移動先 Account を変更します。別 Account への移動で所持品や位置が引き継がれる保証はありません。
- 通常一覧の `Delete` は確認後 Trash へ移します。`Trash` の `Recently Deleted` 一覧では `Restore` で元の Account へ復元できます。既存ワールドは上書きしません。
- Trash の `Delete` → `Delete Permanently`、または `Empty Trash` は取り消せない完全削除です。表示された対象を確認し、バックアップが必要なら先に退避してください。
- 「中断した操作を復旧」が出たら説明を確認します。完全削除の中断では残りの削除を続行する場合があり、すべての復旧が元へ戻す処理とは限りません。

## Texture

`Open Texture Folder` のフォルダアイコンでローカルの配置先を開きます。対応するパックフォルダまたは ZIP を配置し、`Available Textures` の検証結果を確認します。内容は Minecraft Wii U v560 用である必要があります。Java Edition / Bedrock の任意パックを自動変換する機能ではありません。

ゲーム停止中にパックの `Apply` から内容確認を開き、`Apply` で適用します。Current Texture に保存された選択と、実行中に読み込まれた選択を区別します。変更は次回 PLAY に反映されます。外部ファイルの変化は一覧へ反映しますが自動適用はしません。問題時は `Default` を選択、または表示された `Default に復旧` を使用します。

Warning の詳細を読み、実際に問題なく動くことを確認した場合だけ、右クリックの `✓ 問題なく動作することを確認` を使用します。これは警告の確認記録であり、Error の解消や互換性の保証ではありません。

## Graphic Packs

`Open Graphic Pack Folder` のフォルダアイコンで配置先を開き、対応する `rules.txt` を持つローカル Graphic Pack を置きます。Minecraft 向けの一覧で有効・無効と Preset を選択します。保存後は次回 PLAY に反映されます。ゲーム中は変更できません。

パックの要求描画 API / GPU と実際の環境が合わない場合、そのパックは適用されないことがあります。すべての外部パックの正確性・安全性・性能は保証していません。自動ダウンロードストアはありません。

## Session History

`Daily` / `Weekly` / `Monthly` / `Yearly` で期間を選び、前後移動と日付別履歴を確認します。ゲームが稼働している時間を記録し、起動準備や PC のスリープ時間を除外します。記録はローカルの共通履歴で、Account 別・ワールド別の集計ではありません。保存警告 `!` が出た場合は、既存履歴を削除せず[TROUBLESHOOTING](TROUBLESHOOTING.md)へ進みます。

## Settings

| タブ | 操作 |
|---|---|
| General | `Check for Updates on Startup`、`Check for Updates`、Launcher Information。更新は[UPDATING](UPDATING.md)参照 |
| Input | `Physical Controller` を選び、Button Mapping で割当。連続割当、Skip / Cancel / Finish、Stick Deadzone、Controller Input Test を使用。Profile は `読込` / `新しい名前で保存` |
| Graphics | FPS上限・内部解像度、`Graphics API` の Vulkan / OpenGL、GPU・VSync・フィルターなど。変更可能な状態は画面の案内に従う |
| Audio | Audio API、TV / GamePad Output、Volume、Microphone。実行中の反映あり。Microphone はゲームが要求したときに使用 |
| Overlay | FPS 等の表示位置・色・倍率と通知。Friends 通知の有効化だけではオンライン接続しません |

設定は変更時に保存されます。Profile の明示保存など個別操作もあります。保存失敗の表示を確認し、終了前に処理完了を待ってください。Input Test の反応はゲーム内入力の保証ではないため、実際のボタン・スティックも確認します。再接続後に複数の同型デバイスがある場合は対象を選び直してください。非アクティブなウィンドウへの入力は抑制されます。

## Discord Rich Presence

Discord Social SDK を通じて、閲覧ページ、`Playing Minecraft: Wii U Edition` / `In Game`、Launcher 開始時刻等の固定情報を通知します。Account の PNID、ワールド名、保存パスをこの Presence に入れる実装ではありません。[通信と保存](PRIVACY.md)も参照してください。

Discord がない・終了している場合でも Launcher を利用できる設計で、既存の隔離試験で継続利用を確認しています。再接続時は現在の状態を再送します。Discord 側のアクティビティ共有設定によって表示は制限されます。SDK の成功応答だけでプロフィール上の表示成功とは判断していません。共有 ON での外観受入や異常終了後の表示消失時間は未完了です。
