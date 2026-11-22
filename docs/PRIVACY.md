# データの保存と外部通信

[README](../README.md) / [SECURITY](../SECURITY.md)

2026-11-23 のソースを確認した範囲の説明です。Launcher、Cemu Core、Discord Social SDK、WebView2 とサービス事業者の処理を区別します。全パケットの観測や第三者 SDK 内部の監査を完了したものではありません。

## ローカル保存

既定の保存先は `%LOCALAPPDATA%\MCWiiU-Launcher` です。プログラムの既定導入先 `%LOCALAPPDATA%\Programs\MCWiiU-Launcher` とは別です。

| 場所 | 主な情報と目的 |
|---|---|
| `Data/mlc01` | 導入済みゲーム、Update、DLC、セーブ、Account、Mii 等のエミュレーション用データ |
| `Data/settings.xml`・`Data/launcher-*.json` | 入力・描画・音声・Overlay、ネットワークサービス選択、Launcher 設定など |
| `Data/controllerProfiles` | コントローラーの割当・Profile |
| `Data/Textures`・`Data/graphicPacks` | ローカルパック。適用情報・管理用コピー等も Data 内に保存 |
| `Data/session-history.json` と関連バックアップ | プレイ時間、日付ごとの集計、保存・復旧用状態。Account・地域・GPU・ワールド別の解析情報は記録対象外 |
| `Logs` | ローカル診断記録。別途 Core 等のログにもパスや通信関連情報が含まれる可能性あり |
| `Cache` | キャッシュ、更新ダウンロード・処理用データなど |
| `WebView2` | UI 用ブラウザーエンジンのプロファイル・キャッシュ等 |

Account のインポート・編集では Mii 名、誕生日、地域、メール、識別子等を扱います。Online Files には個体情報・認証関連の機密情報が含まれ得ます。Custom 接続先は取り込んだ `network_services.xml` に基づきます。元の ZIP や保存フォルダを外部共有しないでください。

Session History は稼働時間をローカル保存する機能です。専用の解析サーバーへ履歴を送るテレメトリー機能は、今回確認した Launcher コードには見つかっていません。この事実を、すべての依存物やゲーム通信が外部へ何も送信しないという保証にはしません。

## 外部通信と制御

| 経路 | 扱う情報・目的 | 利用者が確認できること |
|---|---|---|
| GitHub API / 更新 ZIP | `IGNSeed/MCWiiU-Launcher` の Stable Release を HTTPS で照会し、署名/Asset 情報と ZIP を取得。接続先には通常の通信メタデータ・IP 等が見える | General の `Check for Updates on Startup` と手動確認、更新承認。本番鍵・Release は現在未設定/未公開 |
| Foundation 案内 | `Open Official Releases` で既定ブラウザーに正規 Releases を開く | 利用者が明示的にボタンを押し、Setup を手動取得・実行 |
| Discord Rich Presence | Social SDK からデスクトップ Discord へ RPC。固定ページ名、Playing/In Game、開始時刻、アプリ識別子・画像キーを通知。ワールド名・PNID・保存パスは Presence payload に含めない | Discord 側の共有設定と実際のプロフィール表示。Launcher 内に共有 ON/OFF の専用 UI はない |
| Friends / ゲーム通信 | 選択した Pretendo / Custom サービスへ Cemu の認証・Friends 通信。Account 識別・認証関連データ、Mii・表示名・国/地域、相手の ID・状態・申請等を扱う。Friends は Core の準備後、条件が整えば PLAY 前にも接続開始 | Account のサービスと準備状態、Friends の接続表示。サービスの規約・プライバシーも別途確認。申請管理は Pretendo に限定 |
| WebView2 | 同梱 UI の表示、ローカルプロファイル保存 | UI の HTTP/HTTPS リクエストと外部ナビゲーションを拒否する実装。ただし Runtime 自体の更新・診断等は Microsoft の処理で、全通信の非発生は保証していない |

Friends の Offline 選択はゲーム側サービスの選択です。GitHub 更新確認や Discord Presence を一括停止する設定ではありません。オンライン用情報が不足する場合は準備不足を表示します。接続成功や全オンライン機能の利用を保証しません。

Presence 統合は RPC のみを使い、Launcher の Discord OAuth ログイン・チャット機能を実装していません。Discord が利用できない場合は通知を送れなくても Launcher は継続利用できる設計です。SDK や Discord サービスが別途扱う情報・保持期間は、[Discord Privacy Policy](https://discord.com/privacy)および適用される SDK 条件を確認してください。WebView2 は [Microsoft Privacy Statement](https://privacy.microsoft.com/privacystatement)、GitHub は [GitHub Privacy Statement](https://docs.github.com/en/site-policy/privacy-policies/github-general-privacy-statement)の対象でもあります。

## 保持・提出・削除

通常のアンインストールはユーザーデータを保持します。ゲームデータだけの削除もセーブ・Account・設定等を保持します。World / Account の明示的な削除操作は別です。完全削除を判断する前に必要なデータをバックアップしてください。

ログの自動外部提出機能は、今回確認した Launcher のコードにはありません。利用者が報告用に提出する場合は、[伏字化の注意](TROUBLESHOOTING.md#解決しない場合の報告)に従います。ローカルログの全面的な秘密情報除去と、全ネットワーク経路の実測受入は未完了です。

専用の問い合わせメール・非公開窓口は未設定です。Issues も現時点で無効のため、問い合わせ経路の整備は配布前の課題です。機密情報を送るための仮の宛先は用意していません。
