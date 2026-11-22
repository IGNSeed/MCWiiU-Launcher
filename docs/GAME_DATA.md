# Minecraft のゲームデータ導入

[README](../README.md) / [インストール](INSTALLATION.md)

対象は **Minecraft: Wii U Edition + Update v560** です。BASE と Update は必須、DLC は任意です。ゲームデータや鍵は付属しません。自身が適法に用意したデータを使用してください。ゲームの配布サイト・鍵の取得先の案内は行いません。

## 対応形式

`code`、`content`、`meta` を持つ展開済みのタイトルフォルダを、BASE / Update / DLC ごとに用意します。フォルダそのもの、またはその中の `meta/meta.xml` を選択できます。

```text
タイトルフォルダ/
├── code/
│   ├── app.xml
│   ├── cos.xml
│   └── Minecraft.Client.rpx   (BASE と Update で必須)
├── content/
└── meta/
    └── meta.xml
```

これは必須構造の例です。元の `content` などの全データも必要です。DLC でも3ディレクトリ、`app.xml`、`cos.xml`、`meta.xml` を検査しますが、RPX は要求しません。XML のタイトル ID・版・地域の整合性、ファイルの存在、安全なパスを確認します。ファイルを自作・改変して検査を通すことは避けてください。

JPN / USA / EUR のタイトル ID を受理します。BASE と Update の地域を一致させてください。Update は **v560 のみ**です。他地域の DLC は保持できますが、ゲーム内での地域間マウント互換性は保証しません。地域別の全ゲーム受入は未完了です。

Game Data の導入画面は WUD / WUX / WUA、暗号化済み配布形式、ゲーム ZIP や単独 RPX を直接インポートする入口ではありません。`meta.xml` だけをコピーしてもゲームは導入できません。管理中の MLC と重なるフォルダやリンク・ジャンクションもソースに指定できません。所有する原本は管理ストレージの外に保持してください。

## 初回導入

1. Home の `PLAY`、または `Settings → Game Data → Install Game` で `Minecraft Wii U Setup` を開きます。Core の準備・別操作の終了を待ってから進めます。
2. `BASE` で `フォルダを選択` または `meta.xmlを選択` を押し、BASE を選びます。解析が終わり、表示されたタイトル・地域・版が正しいことを確認して `次へ`。
3. `Update v560` で同じ地域の Update を選び、同様に確認して `次へ`。
4. `DLC` は任意です。導入しなければ `スキップ` を選びます。
5. `内容確認` で導入するデータと保持されるデータを確認し、`実行する` を押します。
6. コピー・正式配置・後処理が終わるまで待ちます。コピー中は可能な場合だけ `キャンセル` できます。配置・復旧・後処理中は停止できません。
7. `Installation Complete`、`BASE + Update v560`、`The game is ready.` を確認して `Back to Home`。準備完了後の `PLAY` で起動します。

導入中に別ページへ移動しても処理は続きます。上部の `進捗を表示` で戻れます。終了操作は安全なキャンセルまたは処理完了を待ちます。強制終了や原本の移動を避けてください。Standalone 開発ビルドではデータ管理はできますがゲームを起動できません。

## 追加・再導入・削除

入口は `Settings → Game Data` です。稼働中のゲームや保存処理がある場合、変更操作は無効になります。

| 操作 | 内容 |
|---|---|
| `Install Update` / `Install DLC` | 対応するデータを選び、内容確認後に導入 |
| `Reinstall BASE` / `Reinstall Update` | 同じ種類のデータを改めて選び、確認後に管理対象を入れ替え。既存導入にはこちらを使用 |
| `New Region` → `Change Region` | 新しい地域の BASE と一致する Update v560 を選択。旧地域のセーブ・DLC・パック設定は保持。地域間のセーブ変換・コピーはしません |
| `Installed DLC` → `Remove DLC` | 選択したタイトル ID の DLC のみ削除。対象を確認して `削除する` |
| `Uninstall Game Data` | 管理対象の BASE・Update・DLC を削除。内容を確認して `削除する` |

これらのゲームデータ操作は **ワールド・セーブ・Account・Mii・Controller Profiles・Graphic Packs・Textures・設定・WebView2 データを保持**します。World の `Delete Permanently` や `Empty Trash` は別の明示的な削除操作です。

## 保存先と問題発生時

管理データは `%LOCALAPPDATA%\MCWiiU-Launcher\Data` 以下です。ゲームは `mlc01/usr/title/<タイトル種別>/<タイトルID下位>`、セーブは `mlc01/usr/save/00050000/<タイトルID下位>` 以下の Account ごとのデータに保持されます。Account・Mii 等のシステムデータも MLC 内にあり、MLC 全体の削除は復旧手順ではありません。実際の World の場所は `World → Open World Folder` で確認できます。

`AlreadyInstalled` は再インストール操作を使用、`UnsupportedVersion` は Update v560 を確認、`RegionMismatch` は BASE/Update の地域を確認します。空き容量不足やアクセス拒否では、原本を保持して容量・権限・使用中のアプリを確認します。

中断時は操作記録を照合して復旧します。`RecoveryRequired` または「復旧が必要です」と表示されたら、既存データと操作記録を削除せず保全し、Launcher を正常終了して状況を記録してください。無条件の再試行、ジャーナル削除、セーブ削除はしません。詳細は[TROUBLESHOOTING](TROUBLESHOOTING.md)を参照してください。
