# セキュリティと配布確認

MCWiiU Launcher は 2026-11-23 に [0.4.0 Stable](https://github.com/IGNSeed/MCWiiU-Launcher/releases/tag/v0.4.0) を公開しました。本番 Ed25519 公開鍵を組込み済みです。旧開発版の自動移行は正式サポート外です。

## 正規の配布元

[IGNSeed/MCWiiU-Launcher の Releases](https://github.com/IGNSeed/MCWiiU-Launcher/releases)を確認してください。所有者・リポジトリ・対象版・ファイル名を照合し、第三者ミラーや広告を取り違えないでください。[正式 Setup](https://github.com/IGNSeed/MCWiiU-Launcher/releases/download/v0.4.0/MCWiiU-Launcher-0.4.0-Setup-x64.exe) を取得できます。

Setup には **Authenticode 署名を使用しません**。Windows による発行元認証があるとは案内していません。SmartScreen / Smart App Control の警告やブロック時に、Windows のセキュリティ機能を無効化しないでください。判断できないファイルは実行を止めます。

SHA-256 は内容の一致確認です。同じ配布場所のハッシュ値だけでは発行元の真正性まで保証できません。未署名 Setup の独立した信頼確認の運用は、一般配布前に確定する必要があります。

## 更新の検証

通常の Launcher 更新は Ed25519 署名付き Manifest、GitHub Stable Release の Asset 情報、ZIP の SHA-256 / サイズ / 全ファイル一覧、パス・互換性検査を行います。利用者の `Update Now` 承認後に進め、起動 Readiness 失敗時は検証できる previous へ Rollback する方式です。

Foundation 更新は利用者が正規 Releases から Setup を取得して実行する手動方式です。自動 Setup 実行は行いません。内部署名検証は未署名 Setup 自体に対する Windows の発行元認証の代替ではありません。本番鍵と初回配布ファイルの検証は完了していますが、次版への実更新試験は未実施です。検証失敗は受理せず停止する設計です。

[更新・復旧ガイド](docs/UPDATING.md)に従い、署名検証を迂回したり、Manifest・current・pending を改変して更新を強行したりしないでください。

## 問題の報告

**Private Vulnerability Reporting は現在無効です。非公開報告窓口・専用メールも未設定です。** 2026-11-23 にリポジトリ設定を読み取り確認しました。存在しない秘密の連絡先や応答期限を案内することはできません。

[GitHub Issues](https://github.com/IGNSeed/MCWiiU-Launcher/issues)も現在無効です。一般配布前に非公開の連絡手段と受付体制を整備する必要があります。窓口が整うまでは、悪用可能な詳細や機密情報を公開投稿せず保全してください。公開報告が利用可能になった場合も、通常不具合の非機密な再現情報だけを扱います。

秘密鍵、API Token、認証情報、ゲーム鍵、個人情報、Account 原本、セーブ・MLC 全体を公開 Issue やログ添付に含めないでください。診断記録は利用者が確認し、パス・ユーザー名・ID・IP 等も必要に応じて伏字化します。[報告情報の注意](docs/TROUBLESHOOTING.md#解決しない場合の報告)を参照してください。

依存ライブラリの全 CVE 評価、全通信/ログの監査、クリーン PC の全面受入・次版への実更新試験は未完了です。文書整備はこれらの安全性を保証するものではありません。
