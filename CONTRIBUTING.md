# MCWiiU Launcher への貢献

対象は Windows x64 / Minecraft: Wii U Edition v560 です。変更前に [BUILD](BUILD.md)、[CODING_STYLE](CODING_STYLE.md)、[LICENSE](LICENSE.txt) を確認してください。Cemu の著作権表示と MPL-2.0、第三者の原文通知を維持します。

小さく目的の明確な変更を歓迎します。UI と Core の責務を分け、上流 Cemu の更新を取り込める構造を維持してください。無関係な一括整形や公開 API の変更を避け、既存の改行・文字コードを維持します。

PR には問題、変更後の動作、検証した範囲・環境・未検証事項を記載します。実際に実施していない OS・実ゲーム・物理入力・音声の受入を成功として記載しないでください。セーブと既存設定を保護し、検証用コピーを使用します。

通常の報告先は [このリポジトリの Issues](https://github.com/IGNSeed/MCWiiU-Launcher/issues)を予定していますが、現在は無効です。受付設定の整備は未完了です。上流 Cemu の Discord / Matrix / Wiki を Launcher 専用の窓口として案内しません。報告内容は [TROUBLESHOOTING](docs/TROUBLESHOOTING.md)、機密性のある問題は [SECURITY](SECURITY.md) を確認してください。

ゲーム本体、鍵、Account・セーブ、SDK 原本、生成バイナリ、ローカル検証ログを PR へ含めないでください。必要な製品ドキュメントは docs 内の公開ガイドに限定し、調査資料やローカル規約は公開しません。

以下は上流から引き継いだコード投稿方針です。原文を維持しています。

#### AI-generated contributions:

For new contributors we ask that all code submitted is written and understood by a human. You can use AI for planning, designing, reviewing and for asking questions about the codebase, but the code itself needs to be written by you. As a small exception you can use IntelliSense-style AI code autocompletion for pure boilerplate code as long as it's only a small part of your submission. To further clarify, when we ask for "human written" that excludes letting an AI write the code and then paraphrasing it. In other words, we are asking for human effort.

Why this policy exists:

We have relatively low reviewing capacity and requiring human-written code increases the quality and trustworthiness of submitted pull requests. There are also general concerns with AI usage in emulation:
- LLMs tend to make up solutions that work on the surface but are generally not accurate in the emulation sense
- There is evidence that LLMs have been trained on leaked proprietary SDKs and we cannot verify the origin of the knowledge. This is especially a problem for core emulation logic

Please keep these points in mind when contributing to Cemu. Contributions that do not follow this policy may be rejected.
