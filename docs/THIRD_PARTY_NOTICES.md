# ライセンス・第三者通知・対応ソース

[README](../README.md) / [LICENSE.txt](../LICENSE.txt) / [BUILD](../BUILD.md)

## Cemu と MPL-2.0

MCWiiU Launcher は [cemu-project/Cemu](https://github.com/cemu-project/Cemu) を基盤とする非公式プロジェクトです。Cemu とその貢献者の著作権表示、MPL-2.0 の原文と適用条件を維持します。リポジトリの [LICENSE.txt](../LICENSE.txt) は変更していません。

[MPL-2.0](https://www.mozilla.org/en-US/MPL/2.0/) の対象ソースと変更には同ライセンスの条件が適用されます。実行形式の配布時には、対象ソースを入手できるようにし、その入手方法を受領者へ案内する必要があります。第三者ライブラリにはそれぞれの条件があり、ゲーム・画像・商標の権利を MPL が許諾するわけではありません。

## 原文通知の場所

Phase 13 のローカル Native パッケージでは **125件の原文通知**と `license-inventory.json` を採取しました。生成工程は [LauncherLicenses.cmake](../src/launcher/LauncherLicenses.cmake) と [installer](../src/launcher/installer/PreparePackage.ps1) です。件数は現行依存集合の検証結果であり、将来版の固定件数ではありません。

ビルドの `bin/licenses/`、ZIP の `licenses/`、導入先の `versions/<版>/licenses/` に原文を置く方式です。同階層の `license-inventory.json` が component、元 source の相対パスと SHA-256、通知ファイルのパスと SHA-256 を対応付けます。欠落・改変した通知を文章の要約で置き換えず、パッケージ生成を停止します。v0.4.3の正式ZIPにも原文通知とinventoryを含め、元ファイルと個別SHA-256を照合しています。

| 依存物 | 原文通知（`licenses/` 内）・対応 |
|---|---|
| Cemu / MPL-2.0 | `Cemu-LICENSE.txt` ← リポジトリ `LICENSE.txt` |
| Discord Social SDK 1.11.24259 | `Discord-Social-SDK-NOTICES.txt` ← 認可された公式 SDK の `License-Notices.txt`。SDK 契約条件も別途適用 |
| saucer 8.0.5 / MIT | `saucer-LICENSE` ← 固定取得 source の LICENSE |
| coco、rebind、ereignis、flagpp、polo、lockpp、glaze、saucer-fill | 各 `<component>-LICENSE.txt` ← CPM 取得 source の原文 |
| WebView2 SDK | `WebView2-LICENSE.txt` ← Microsoft NuGet SDK の LICENSE。Evergreen Runtime 全体は同梱しない |
| OpenSSL 3.5.9 | `openssl-copyright.txt` ← 実際の vcpkg installed share の copyright |
| curl 8.23.0 / libpng 1.6.59 / Expat 2.10.0 | `curl-copyright.txt` / `libpng-copyright.txt` / `expat-copyright.txt` ← 各 installed copyright |
| wxWidgets、zlib、libzip、Boost 等 | 各 `<component>-copyright.txt`。実 inventory が採取対象の正本 |
| cubeb / ZArchive / ImGui | `cubeb-LICENSE.txt` / `ZArchive-LICENSE.txt` / `imgui-LICENSE.txt` ← 各 submodule 原文 |
| cubeb 内 Speex resampler | `speex-<source名>.txt`、6原文コメント。source 全体と抽出通知を別々にハッシュ記録 |
| Vulkan-Headers | `Vulkan-Headers-LICENSE.md`、`Apache-2.0.txt`、`MIT.txt` |
| ih264d | `ih264d-NOTICE.txt`、`ih264d-Apache-2.0.txt` |
| FontAwesome 4.7.0 | `FontAwesome-COPYRIGHT.txt`、`FontAwesome-OFL-1.1.txt` |

vcpkg port の再利用元表示は source 内の `LICENSE-vcpkg.txt` も維持しています。ビルド専用ツール、OS 付属フォント/API、実行時に同梱するコードを区別してください。上表と125通知だけで、全 vendored 断片や全契約条件の監査が完了したとはしていません。

## 対応ソースの入手

v0.4.3の対応ソースは [Commit `3d9339b09fae4f587b12acb5fed8a8e0a679f9d3`](https://github.com/IGNSeed/MCWiiU-Launcher/tree/3d9339b09fae4f587b12acb5fed8a8e0a679f9d3)、固定Tagは `v0.4.3` です。`git checkout 3d9339b09fae4f587b12acb5fed8a8e0a679f9d3` と `git submodule update --init --recursive` で対応するsubmoduleと取得先を復元します。[同CommitのBUILD](https://github.com/IGNSeed/MCWiiU-Launcher/blob/3d9339b09fae4f587b12acb5fed8a8e0a679f9d3/BUILD.md)を参照してください。Release説明にも正確なCommitと入手方法を記載しています。最終配布はクリーンソース用キャッシュの入力と照合し、未公開wx差分と診断変更を含めていません。これは下記の素材許諾・契約・依存評価の未確認事項を解消したことを意味しません。

## SDK・ゲーム・素材の境界

Discord SDK は認可された Developer Portal から取得します。公開 Git に SDK の原本・ヘッダー・import library・アーカイブを同梱しません。配布は統合に必要な DLL と原文通知を対象としますが、適用される全契約条件・供給運用の最終確認は未完了です。[Discord Social SDK Terms](https://support-dev.discord.com/hc/en-us/articles/30225844245271-Discord-Social-SDK-Terms)も確認が必要です。

ゲーム本体、Update、DLC、セーブ、Wii U システムファイルは配布物に含めません。現在の埋込画像は維持していますが、第三者素材の出所・再配布許諾の証拠は未確定で、一般配布の残課題です。これらの画像を文書の新しいスクリーンショットとして転載していません。ソース公開や本ドキュメントは、素材の許諾問題が解決した証拠ではありません。
