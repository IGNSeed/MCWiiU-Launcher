# MCWiiU Launcher の Windows x64 ビルド

[README](README.md) / [貢献方法](CONTRIBUTING.md) / [第三者通知](docs/THIRD_PARTY_NOTICES.md)

対象は Windows 10 / 11 x64、MSVC、Ninja の Release ビルドです。Launcher は C++23、Cemu Core は C++20 です。Launcher 0.4.0 / Foundation 1.0.2 / Updater Protocol 2 を使用します。Linux・macOS・FreeBSD 向け Cemu の構成はこの Launcher の対応手順ではありません。上流については [Cemu](https://github.com/cemu-project/Cemu) を参照してください。

**再現性の制限:** Phase 13 の Native / Standalone 増分ビルドは成功していますが、既存キャッシュ、ローカル取得 SDK、保護された未公開 wx 差分を含む同一開発ホストでの検証です。クリーン環境での完全再現性・配布バイナリとのソース一致は未検証です。Phase 14 は文書変更で、ゲーム・製品の再ビルドを実施したとは扱いません。

## 開発環境

| 項目 | 現行構成・条件 |
|---|---|
| Visual Studio / MSVC | C++ デスクトップ開発、x64 tools と Windows SDK。既存キャッシュの compiler は MSVC 14.51.36231、Visual Studio 18 Community。C++23 を扱える環境が必要 |
| CMake / Ninja | Launcher の最低 CMake は3.25（Core 側3.21.1）。既存ホストの実測は Visual Studio 同梱 CMake 3.31.6-msvc6 / Ninja 1.12.1 |
| Windows SDK | 既存ホストに10.0.26100.0を導入。Windows x64 / RC / WinHTTP / Crypto / COM 等を使用。新規ホストでの互換性は別途確認 |
| Git | submodule を含めて取得。対応 revision を維持し、無条件の上流 main 更新はしない |
| vcpkg | `dependencies/vcpkg` と tracked overlays / `vcpkg.json`。Native は `x64-windows-static`、既存 cache を再利用 |
| saucer / WebView2 SDK | [Launcher CMake](src/launcher/CMakeLists.txt) で saucer 8.0.5 の archive と SHA-256 を固定。WebView2 backend、SDK 1.0.3650.58 は saucer の NuGet 経由。Runtime は別途必要 |
| Discord Social SDK | 認可された公式 C++ SDK。下記の版・DLL hash と原文 notice を検査 |
| Inno Setup | Setup 作成には **7.1.1**。ISCC の SHA-256 は `e0f9f1dc32032650c6629f6dee41eb6d20ba140e4e3c07c25d8a24f9ba781644` に固定 |

既存 CMakeSettings.json は Visual Studio の一般構成で、Launcher 用 CMakePresets.json はありません。`--preset` を前提にしないでください。初回依存取得にはネットワーク・容量・時間が必要です。通常は既存オブジェクトと依存キャッシュを使用します。

## ソースと Discord SDK

```powershell
git clone --recurse-submodules https://github.com/IGNSeed/MCWiiU-Launcher.git
cd MCWiiU-Launcher
git submodule update --init --recursive
```

配布物を再現する場合は、将来の Release に示される正確な Commit SHA へ対応付けます。現時点で正式 Release Tag はありません。指定された Cemu fork 基点は `3f8dd913cfd4c8247ad65ab41ba6ef0a55926769` で、現在の Launcher の HEAD を意味する値ではありません。

Discord SDK は [Discord Developer Portal](https://discord.com/developers/applications) で認可されたアプリの Social SDK ダウンロードから取得します。リポジトリへ SDK をコピー・Commit しないでください。既定のローカル配置先は `.local/dependencies/discord-social-sdk`。別配置は `MCWIIU_DISCORD_SDK_ROOT` と `MCWIIU_DISCORD_SDK_NOTICES` で指定できます。

必須ファイルは `include/discordpp.h`、`lib/release/discord_partner_sdk.lib`、`bin/release/discord_partner_sdk.dll`、原文 `License-Notices.txt` です。現行の SDK は **1.11.24259**、DLL の SHA-256 は次の値です。

```text
0688b44b738148cb0df9beca00a64f2b8b351f87a7af39035bc378008df34f0d
```

不一致・不足は構成を停止します。検査を無効化せず、版・互換性・契約を確認してください。CMake は SDK を自動取得・代用品へ置換しません。配布対象は統合 DLL と原文 notice であり、SDK 全体の再配布や CI 供給の認可を推定しません。

## Native Release と Standalone Release

| 構成 | ソース・出力 | 用途 |
|---|---|---|
| Native | ルート CMake、`ENABLE_NATIVE_LAUNCHER=ON`、`out/build/Release` | Cemu Core と同一プロセスでゲームを動かす製品構成 |
| Standalone | `src/launcher` CMake、`out/build/LauncherRelease` | Core のない UI/更新等の開発構成。ゲームを起動できず、Core 必須機能も利用不可 |

両方の本体出力名は `bin/MCWiiU Launcher.exe` です。内部 target は `LauncherShell` のままです。Bootstrapper は `MCWiiU-Launcher`、Updater は `MCWiiU-Updater`、Setup helper は `MCWiiU-SetupGuard`、署名ツールは `MCWiiU-SigningTool`。後者は EXCLUDE_FROM_ALL のため明示的にビルドします。

以下の target 指定は Phase 13 の増分ビルドで使用したものです。**既存の構成済みキャッシュ**と、x64 MSVC / CMake / Ninja の環境が必要です。

```powershell
cmake --build out/build/Release --target LauncherShell CemuBin --parallel 8
cmake --build out/build/LauncherRelease --target LauncherShell MCWiiU-SigningTool --parallel 6
```

このホストではローカルの `.local/temp/incremental-integration.cmd`、`.local/temp/incremental-launcher.cmd` と署名ツール target を使うラッパーで環境を初期化しています。これらは公開リポジトリには含まれないため、新しい clone に存在するとは案内しません。既存の `.local/temp/build-release.cmd` は CemuBin の構成/ビルド入口で、単体では Native Launcher の構成指定を代替しません。

構成に必要な実在オプションは Native の `-S . -B out/build/Release -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_NATIVE_LAUNCHER=ON -DVCPKG_TARGET_TRIPLET=x64-windows-static`、Standalone の `-S src/launcher -B out/build/LauncherRelease -G Ninja -DCMAKE_BUILD_TYPE=Release` です。これらは既存構成スクリプトと CMakeCache に対応しますが、短縮した引数列だけで新規環境が完全再現できるという意味ではありません。

現ホストではさらに依存 source/cache・NuGet の配置と、日本語 MSVC のヘッダー依存対策の compiler launcher / make-rule override を使っています。使用中の補助ファイルを削除せず、cache の設定を維持してください。Standalone も既存 Native の `out/build/Release/vcpkg_installed/x64-windows-static` の OpenSSL・libzip・原文通知を参照するため、Native の依存準備が先です。正確な初回構成と SDK 供給の独立再現は配布前の残課題です。

`MCWIIU_DEVELOPER_DIAGNOSTICS` は通常 Release で **OFF** を維持します。保護対象の既存ソースやユーザーデータをリセットしたり、別の構成を大量に作ったり、キャッシュを毎回消したりしないでください。ビルド前後で空き容量を確認します。

## パッケージと署名

以下は既存実装と Phase 13 の隔離試験に基づく工程です。本番鍵が未生成・公開鍵が未設定なので、**現状で正式 Setup を生成・配布できる手順ではありません**。ここでは本番鍵生成、Release / Draft / Tag 作成、アップロード、workflow 実行を行いません。

| 順序 | 実在するツール・入力・出力 |
|---|---|
| 1 | `installer/PreparePackage.ps1 -BinaryDirectory <Native bin> -OutputDirectory <新規.local出力先>`。payload・ZIP・package-plan.json を生成。Asset ID はまだ不要 |
| 2 | 別途承認された配布工程で確定 ZIP の GitHub Asset metadata を採取。ID、名前、Tag、size、digest、browser_download_url を保持。ローカル推測値は使わない |
| 3 | `installer/BuildPackage.ps1 -PreparedDirectory <準備先> -ReleaseAssetId <実ID> -AssetMetadata <metadata JSON>`。確定 ZIP と照合して manifest.unsigned.json を生成 |
| 4 | `MCWiiU-SigningTool.exe sign manifest <署名鍵入力> <公開鍵ファイル> <unsigned manifest> <新規署名出力先>`。正確な bytes を署名し manifest.json / manifest.sig を生成 |
| 5 | `installer/BuildFoundation.ps1 -BinaryDirectory <bin> -SigningTool <tool> -OutputDirectory <新規.local出力先>`、同 tool の `sign foundation` で foundation.json / foundation.sig を生成 |
| 6 | 同一 bundle に ZIP、payload、manifest.json/.sig、foundation.json/.sig をそろえる。公開 asset の Manifest 名は update-manifest.json / update-manifest.sig という契約に合わせる |
| 7 | `installer/BuildSetup.ps1` に BinaryDirectory、BundleDirectory、OutputDirectory、InnoCompiler、SigningTool、PublicKeyFile を明示。全署名・ZIP・payload・基盤 EXE と固定 ISCC を検証してから Setup を生成し、入力を再検証 |

`installer/` は [src/launcher/installer](src/launcher/installer/PreparePackage.ps1) の略です。実パラメーターは各スクリプトの `param` を参照してください。出力は workspace の `.local/` 内の新しいディレクトリに限定します。署名後の ZIP/Manifest は変更せず、変更が必要なら新規取引として再準備・再署名します。

鍵の入力欄は構文説明用で、本番秘密鍵の実体・保存先は記載していません。署名は認可された管理環境でのみ行い、秘密値をソース・コマンド履歴・ログへ貼り付けないでください。将来の本番鍵導入は別工程です。

既存の署名ツールには読み取りの `identity`、`fingerprint <公開鍵>`、`verify-manifest <公開鍵> <Manifest> <署名>`、`verify-foundation`、`verify-bundle <公開鍵> <bundle> <bin>` があります。本番生成コマンドをこの文書の確認目的で実行しないでください。Setup は Authenticode 署名を使わない方針です。

原文通知・秘密情報・バイナリ・ソース対応・Runtime・入力/音声/描画・更新/復旧の検査を経てから、別途配布可否を判断します。125件の通知は全法的条件の完了を保証しません。既存 Cemu workflow は Launcher の正式配布 CI ではなく、旧公開 job は無効化済みです。

## 検証と貢献

変更範囲に応じて静的確認、対象 target のビルド、隔離試験を選びます。ゲーム原本・セーブ・MLC・Account・SDK 原本は保護し、物理デバイスやクリーン PC の未試験を PASS にしません。文書変更の検査で実ゲームや OS の受入が完了したとは扱いません。

`git diff --check`、リンク・製品表記・コマンドのソース照合と、許可したパスだけのステージングを行います。生成物、SDK 原本、鍵、ユーザーデータ、ローカル検証物を公開しないでください。規約は[CONTRIBUTING](CONTRIBUTING.md)と[CODING_STYLE](CODING_STYLE.md)を参照してください。
