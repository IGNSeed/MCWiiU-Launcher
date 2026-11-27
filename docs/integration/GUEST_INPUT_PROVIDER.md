# Native Guest Input Provider — ABI 1.x

## Phase 5.3: optional ABI 1.2

ユーザーの個別承認に基づき、開発Providerへ軽量Pollを追加する。製品番号0.4.3と既存GUIは維持する。
既存1.0/1.1の構造体・API・buffer検査を変更せず、Guestの必須importにはしない。
VERSION=0x00010002、追加CapabilityはMCW_INPUT_CAP_POLL=64、全capabilities=127。

    uint32_t MCWInputPoll(uint32_t session, uint32_t consumer,
        uint32_t expectedResetSerial, uint32_t expectedCaptureSequence);

既存mutex内でLease失効とtokenを確認し、有効な場合だけLeaseを更新する。
queueが空かつresetSerial/captureSequenceが一致するとOK、変化や待機eventがあると
追加resultのMCW_INPUT_PENDING=7を返す。古いtokenはINVALID_CONSUMER。
Pollはeventを消費せず、Guest pointerを受け取らず、Guest RAMを読み書きしない。
Drain/Capture/Query等のVirtualQuery・bounds・alignment検査はそのまま適用する。

ConsumerはCapabilityとoptional exportの両方を確認する。現在のCaptureと要求が一致し、
右Shift通知のopt-inが確立した場合だけPollを使う。PENDING時は同じ有限Drain経路へ進み、
未知resultやtoken失効時はCapture解除と再登録の安全経路へ戻す。Capture変更はPollで省略しない。
Focus、Overflow、Reset、短いdown/up、session終了の処理・二重配送防止を維持する。
旧Providerまたはexport不足では既存Drain経路を使う。poll頻度自体は下げない。

Provider 46 checks、既存Provider 103095 checks、右Shift47 checks、実HLEのread-only RAM上Poll、
実routing/loader componentはPASS。これはHost componentの結果であり、PPC実行とは区別する。
既存Release cacheのLauncherShell差分buildはPASS。既存NativeGameHostのD9025警告は残る。
Stable、current.json、Updater、正式配布物は変更しない。実ワールドでの比較はTaneClientの
docs/validation/IN_WORLD_PERFORMANCE.mdへ記録し、過去の検証表を今回の結果で上書きしない。



## ABI 1.1 — Phase 5.1の右Shift専用通知

開発Launcher製品番号は0.4.3を維持し、独立Provider ABIを0x00010001へ更新する。
以下のABI1.0本文は導入時の履歴。本節を現行の追加契約とする。
既存module/5 Export/header64/event48/queue256/BE/Guest RAM/Session/Lease/Capture ackの値と意味は維持する。
既存Consumerはopt-inしなければ新Eventを受け取らず、右Shiftも従来のKeyboard入力として扱う。

追加CapabilityはMCW_INPUT_CAP_MENU_TOGGLE=32、全capabilities=63。追加EventはMCW_INPUT_MENU_TOGGLE=10。
optional Exportの正式宣言は次のとおり。

    uint32_t MCWInputToggle(uint32_t session, uint32_t consumer, uint32_t enabled, void* output, uint32_t bytes);

enabledは0/1。有効なsession/consumer、header出力buffer、focusを確認し、成功時は標準64byte headerでackする。
未知enabled/非focusのenable要求はINVALID_REQUEST、古いtokenはINVALID_CONSUMER。
成功ToggleもLeaseを更新する。opt-in状態は通常のCaptureとは独立する。

Game Viewがactiveかつ有効Consumerの明示opt-in中だけ、VK_RSHIFT=0xA1のdown edgeを通知する。
Event10はcode=0xA1/value=1/flags=0、他fieldは0とsession内のsequence/timestamp。
WM_KEYDOWN/UPのVK_SHIFTは既存MapVirtualKeyWのscan code正規化を使い、左Shift=0xA0と区別する。
repeat、opt-in前からheld、Focus復帰後のheldは通知せず、upを確認して再armする。

予約右Shiftは通常KEY Event/modifierから除外し、CemuHostのstate更新とWindowSystemのpoll双方の
LegacyKeyAllowedでKeyboard Controller Mappingへの二重配送を防ぐ。
非Capture中も他キーの通常mappingを維持し、全Keyboard/Text Captureを保持しない。
global hook/他app監視/GetAsyncKeyState/Host pointer公開は追加しない。
Focus Lost、Unregister、Lease失効、Overflow、Session終了、capture lostで予約解除する。
通常Capture変更は予約を維持する。解除時heldはupまでlegacy復帰を保留し、他キーは個別に復帰する。
GuestはresetSerial/focus/session/consumerをrecordより先に検査し、boundaryの古い通知を破棄して再opt-inする。

新ExportはProvider allowlist/HLE登録へ追加するが、Guestの必須importにはしない。
旧Provider1.0/Export不足は専用通知を使わずController開閉と既存Native入力へ戻る。
ProviderなしStableはController/Touchへfallbackする。TaneClientの製品Registryは0件。

Host Provider47 checks、実HLE/既存routing/loader component、新Consumer39 checks、旧Native778 checksがPASS。
Native Release LauncherShell/CemuBinの既存cache差分buildはPASS。
既存NativeGameHost compiler optionのD9025警告は残る。新ProviderのW4/WX componentは警告0。
実UI、PPC Guest API、物理Controller、実ゲーム入力はNOT RUN。長時間/FPS/安定性はSKIPPED。
Stable/current.json/Updater配布物は変更せず、開発EXEを自動runtime deployしていない。
Stable/開発版のKnown Folder/HKCU/Data共有はユーザー許可済み。同時起動は禁止し、完全隔離を要求しない。

Phase 3.5.1で追加したLauncher独自のoptional Host-to-Guest入力API。
Native Game ViewのPC Mouse / KeyboardをWii U Guestへ配送する。
TaneClient固有のGUI・描画・ゲーム定義を持たない。
製品番号は0.4.3を維持し、ABIは独立した1.0。未公開の開発sourceであり、
Stable配布物・Updater・インストール済みLauncherを更新するものではない。
TaneClientへの接続と実入力受け入れはPhase 3.5.2で行う。

## 実装と利用条件

```text
NativeGameHost Windows messages
    -> CemuHost (active game view / screen image area)
    -> guestinput::Provider (bounded queue / capture / session)
    -> mcwiiu_input HLE exports
    -> PPC32 Big Endian guest application
```

- 正式な宣言は[src/input/GuestInputAbi.h](../../src/input/GuestInputAbi.h)。追加の依存libraryはない。
- 実装は`src/input/GuestInput.{h,cpp}`、HLE境界は
  `src/Cafe/OS/libs/mcwiiu_input/`。Native統合は`CemuHost` / `NativeGameHost`。
- Native Launcherの通常title sessionでだけProviderを有効化する。Standalone GUI、通常Cemu、
  Mii初期化専用sessionでは公開しない。通常のCore loader / COSModule登録機構を利用する。
- Global hook、ControllerからのMouse変換、仮想cursor、OS cursor移動、Windows API代理実行はない。
- TV Native Game Viewだけが現在の実入力source。ABIのdisplay値は0=TV / 1=DRCだが、
  Native側にDRC用Game Viewは追加していない。DRCからの実入力対応を意味しない。
- WindowsのGame View focusとWii U Guestのforeground状態は別。headerの`focused`は前者。

## Optional moduleの検出

module名は`mcwiiu_input.rpl`、内部HLE module名は`mcwiiu_input`。
Native titleを開始すると、Coreの`RPLLoader_LoadCoreinit`でoptional moduleも登録される。
ファイルとしてのRPLを配布・読み込みする方式ではない。

Guestは既存のcoreinit `OSDynLoad_IsModuleLoaded`を使い、**戻り値0かつhandle非0**を確認する。
存在しなければ直ちにGamepad / Touchへfallbackし、未知moduleの強制Acquireを行わない。
`OSDynLoad_FindExport(handle, 0, name, &address)`で下記5関数を解決し、
Queryの成功、magic、major、record size、capabilities、activeも検査する。
返ってきた関数addressが非0という理由だけでProvider存在と判定しない。

現在のCore関数本体を使ったcomponent試験では以下を確認した。

| 条件 | 戻り値 / output |
| --- | --- |
| module不存在時のIsModuleLoaded | 0 / handle=0 |
| module不存在時のAcquire | `0xFFFCFFE9` / handle=0 |
| 存在するProviderの既知関数FindExport | 0 / Guest HLE thunk address |
| Providerの未知関数 / data export / 無効handle | `0xFFFFFFFF` / address=0 |

今回のProviderはexport allowlistを持ち、未知exportを非0の未実装HLE stubへ変換しない。
通常Cemu・過去のLauncher実行ファイルで実際にGuestを起動した検証はNOT RUN。
上表は現sourceの実loader / DynLoad関数本体をdependency/export fixtureで実行した結果である。
Guestは他Coreの具体的なerror値へ依存せず、returnとhandle/addressをともに検査する。
IsModuleLoadedの借用handleを無条件にReleaseしない。Guest側でAcquireした場合だけ所有参照を対にする。

## 全exportと戻り値

```c
uint32_t MCWInputQuery(void* output, uint32_t bytes);
uint32_t MCWInputRegister(uint32_t major, void* output, uint32_t bytes);
uint32_t MCWInputUnregister(uint32_t session, uint32_t consumer);
uint32_t MCWInputDrain(uint32_t session, uint32_t consumer,
                     void* output, uint32_t bytes, uint32_t maxEvents);
uint32_t MCWInputCapture(uint32_t session, uint32_t consumer,
                       uint32_t sequence, uint32_t flags,
                       void* output, uint32_t bytes);
```

上記はPPC32 Guest用C宣言。pointer引数はGuestの32-bit virtual address。
HLE wrapperはすべて32-bit integerとして受け取り、検証後に変換する。
通常のPPC呼出規約でr3から順に引数を渡す。返り値はr3の32-bit値。
64-bit引数、Host pointer、HWND、OS handle、任意call要求はない。

| 値 | 定数 | 意味 |
| --- | --- | --- |
| 0 | MCW_INPUT_OK | 成功 |
| 1 | MCW_INPUT_UNAVAILABLE | Native title sessionがない、またはgeneration/token枯渇 |
| 2 | MCW_INPUT_VERSION_MISMATCH | Registerのmajorが1以外 |
| 3 | MCW_INPUT_INVALID_BUFFER | pointer / alignment / size / drain数が不正 |
| 4 | MCW_INPUT_INVALID_CONSUMER | sessionまたはconsumerが無効・期限切れ |
| 5 | MCW_INPUT_BUSY | 別consumerが登録済み |
| 6 | MCW_INPUT_INVALID_REQUEST | capture flags / sequence / focus条件が不正 |

- Queryは登録不要。成功headerだけでConsumerを所有したことにはならない。
  session非active時はUNAVAILABLEと非active headerを返す。
- Registerのmajor引数は`1`。`0x00010000`を渡さない。単一Consumerだけを許可し、
  成功時headerからsession / consumerを保存する。BUSY時に他Consumerのtokenを借用しない。
- Drainは取得済みeventをqueueから除く。`maxEvents`は1..32。
  `bytes >= 64 + 48 * maxEvents`を満たし、実際には`count`件だけを読む。
- Captureは同期処理。戻り値0と返却headerを同時に検査するとackになる。
  Request sequenceはconsumerごとに正の値から単調増加させる。
- Unregisterはcaptureを解放し、登録・queueを破棄する。
- Query以外の失敗時はoutputを読まない。Buffer検査失敗時には状態を変更しない。
  有効なcall中には既存leaseのexpire処理が行われる場合がある。

## ABIと互換性

全fieldは固定幅32-bit、Guest memory内でBig Endian。floatは使わない。
Host structをGuestへmemcpyせず、各wordを明示的にBEへserializeする。
PPC32 Big Endian Guestからは下記structのnative fieldとして読める。
Little EndianのHost試験はBE decodingを行う。

`version = 0x00010000`（upper16=major1、lower16=minor0）。
major変更では再登録・Guest更新が必要。minorは既存のsize・意味を保つ範囲でのみ拡張し、
新機能はcapabilityで判定する。現ABIはheader64 / event48固定。
未知eventはそのrecordをskipし、未知bitを既知機能として利用しない。
sequenceの飛びはmove coalesceでも発生するため、飛びだけをoverflowと判定しない。

### Header: sizeof=64 / alignof=4

```c
typedef struct MCWInputHeader {
    uint32_t magic, version, headerSize, eventSize;
    uint32_t capabilities, session, consumer, captureSequence;
    uint32_t captureFlags, focused, sequence, count;
    uint32_t resetSerial, active, leaseMilliseconds, timestampFrequency;
} MCWInputHeader;
```

| offset | field | 意味 |
| --- | --- | --- |
| 0 | magic | `0x4D435749` (`MCWI`) |
| 4 | version | `0x00010000` |
| 8 | headerSize | 64 |
| 12 | eventSize | 48 |
| 16 | capabilities | 現在31 |
| 20 | session | Native title generation。0は利用不可 |
| 24 | consumer | 登録token。0は登録なし |
| 28 | captureSequence | 最後に受理したrequest sequence |
| 32 | captureFlags | 現在有効なcapture |
| 36 | focused | 入力可能なNative Game Viewなら1 |
| 40 | sequence | 最後に発行したevent sequence |
| 44 | count | 今回のDrain record数。ほかのcallでは0 |
| 48 | resetSerial | 状態破棄世代。変化したらGuestの全pressed状態を解除 |
| 52 | active | title Providerが存在するなら1 |
| 56 | leaseMilliseconds | 2000 |
| 60 | timestampFrequency | 1000 ticks/second |

capabilities: bit0 Mouse / bit1 Keyboard / bit2 Unicode scalar / bit3 Capture+ack /
bit4 Normalized coordinates。現在は`0x1F`。
IME composition、clipboard、DRC専用sourceのcapabilityは存在しない。

### Event: sizeof=48 / alignof=4

```c
typedef struct MCWInputEvent {
    uint32_t type, sequence, timestampHigh, timestampLow;
    uint32_t flags, display;
    int32_t x, y;
    uint32_t code, value, modifiers, reserved;
} MCWInputEvent;
```

| offset | field | 意味 |
| --- | --- | --- |
| 0 | type | 下表 |
| 4 | sequence | session内の順序。coalesceにより欠番あり |
| 8 / 12 | timestampHigh / Low | Host monotonic millisecondsの上位/下位word |
| 16 | flags | Pointerではbit0=screen image area内。Keyではbit0=repeat由来 |
| 20 | display | Pointer: 0=TV / 1=DRC。非Pointerは0 |
| 24 / 28 | x / y | signed Q16.16 normalized pointer座標 |
| 32 / 36 | code / value | type別の内容 |
| 40 | modifiers | Key / TextでShift1、Ctrl2、Alt4、Super8 |
| 44 | reserved | 0。Guestは無視 |

TimestampはHost steady clock由来であり、Wii UのOS timebaseではない。
`((uint64_t)high << 32) | low`で結合し、同じsession内の間隔・順序補助だけに使う。
Host絶対時刻やGuest clockとの直接比較に使わない。overflow RESETのtimestampは0になる場合がある。

| type | 定数 | code / value・扱い |
| --- | --- | --- |
| 1 | RESET | value: 0=登録、1=overflow、2=capture変更、3=focus lost、4=capture lost |
| 2 | MOVE | value=現在のbuttons bitset。button未押下のHoverも配送 |
| 3 | BUTTON | code:0=Left/1=Right/2=Middle。value=変化後のbuttons bitset |
| 4 | WHEEL | code=signed horizontal delta / value=signed vertical delta |
| 5 | KEY | code=Windows VK、value=1 down / 0 up |
| 6 | TEXT | value=Unicode scalar |
| 7 | FOCUS | value=1 focus / 0 lost |
| 8 | LEAVE | pointer leave。value=現在のbuttons |
| 9 | CAPTURE_LOST | Host mouse capture喪失。押下状態を解除 |

buttons bitsetはLeft1 / Right2 / Middle4。同時押しを保持する。
BUTTONのcodeに対応するbitをvalueから読むとPress / Releaseを判別できる。
Wheelはraw Windows deltaをsigned32として保存し、120未満の値を丸めない。
Guest側で`(int32_t)word / 120.0f`へ変換する。+verticalは奥向き、+horizontalは右向き。
Pointer eventのmodifiersは0。Keyboard capture中のKEY / TEXTでmodifier状態を追跡する。

### Guest buffer検査

- 非NULLの4-byte aligned Guest virtual address、size64..1600のみ。
- Drainでは上記のrecord capacityも必須。掛け算はmaxEvents1..32を検査した後に行う。
- address+sizeの32-bit overflowを拒否する。
- map済みMEM1 / MEM2 data RAMの範囲内に全bufferがあることとCore accessibilityを検査する。
  code/private/system/MMIOを出力先として受け付けない。
- Windowsでは全pageのMEM_COMMIT / writable protectionを確認し、guard/no-access/read-onlyを拒否する。
- session / consumerは現在のProvider登録と一致しなければならない。
- Headerとcount件のrecordだけを書き込む。未使用末尾を有効dataとして読まない。
  outputはcall中有効なGuest RAMとして保持し、他threadと同時書き込みしない。

`sizeof` / `alignof`と主要`offsetof`はpublic headerでstatic_assertし、
Host MSVCに加えてdevkitPPC C17 / C++20 compileでも確認した。

## 座標契約

NativeGameHostは受け取ったclient pointをGame View childのphysical client座標へ変換する。
Wheelのscreen座標は`MapWindowPoints(NULL, child, ...)`で一度だけ変換する。
CemuHostは既存`LatteRenderTarget_getScreenImageArea(..., false)`をeventごとに取得する。
独自のDPI倍率・Framebuffer解像度倍率は掛けない。

```text
x_q16 = (point.x - image.x) * 65536 / image.width
y_q16 = (point.y - image.y) * 65536 / image.height
ImGui.x = x_q16 / 65536.0f * io.DisplaySize.x
ImGui.y = y_q16 / 65536.0f * io.DisplaySize.y
```

画面内判定は左上inclusive / 右下exclusive。黒帯・サイドバーの新規Pressを画面端へclampしない。
サイドバーはGame View input sourceではない。screen image area外で始まったPointer/Pressは配送しない。
Game Viewで始まったdragはOS mouse captureを最後のbutton upまで維持する。
その間の範囲外move / button releaseはsigned範囲外座標・inside=0で配送する。
範囲外値をGuestで画面端へclampして新規Pressにしない。
LEAVEではhoverを解除し、継続中のdragと区別する。CAPTURE_LOSTでは全pressed状態を解除する。

Resize / DPI / fullscreen時は同じimage-area参照を使用する設計。
component試験のphysical extent変換と実WindowsのDPI / fullscreen受け入れは別判定である。

## KeyboardとUnicode

入力sourceはGame View HWNDのKEYDOWN/UP、SYSKEYDOWN/UP、WM_CHAR。
Shift / Ctrl / Altは既存のscancode / extended bitから左右別VKへ正規化する。
Enter、Escape、Tab、Backspace、Delete、矢印、一般Function KeyはVKで表す。
重複downは再queueしない。Guest / ImGui側でrepeatを生成する。

F10・F11・Alt+F4はLauncher / Windows操作として優先するため、Guestの完全なkey集合ではない。
通常Escapeは既存Launcher shortcut。Keyboard capture中はGuestへ渡し、capture解除後は元の操作に戻る。
Launcherを操作する経路としてF10/F11を維持する。diagnostics専用キーは既存挙動のまま。

WM_CHARは登録consumerとfocusedかつKEYBOARD+TEXT captureがある間だけ収集する。
UTF-16 surrogate pairをUnicode scalarへ結合する。
孤立surrogate、NUL・C0 control・DEL、不正な32-bit入力値は捨てる。
Enter / Tab等はKEYとして処理し、文字として挿入しない。
日本語BMPと補助平面文字のserializeはcomponentで確認済み。
IME composition UI、clipboard、全IMEの確定文字経路は今回の対応保証に含めない。

ゲームのNativeTextInput / swkbdがactiveな間はProviderのfocusとcaptureを解除し、
文字をGuest Providerへ配送しない。Launcherの別画面のテキスト入力を収集しない。
入力可否はswkbdのactive値だけを読み、各Mouse eventで全文snapshotをコピーしない。

## Consumer、Capture、二重配送防止

capture flagsはMOUSE=1 / KEYBOARD=2 / TEXT=4。TEXTにはKEYBOARDも必要。
未知bitはINVALID_REQUEST。focus=false中の非0 capture要求もINVALID_REQUEST。
Mouse / Keyboardは独立して所有でき、物理GamePad / Pro Controllerをcaptureしない。

1. Queryを検査し、Register(1)を実行してsession / consumerを保存する。
2. GUIを入力可能にする**前**にCapture(session, consumer, ++request, flags)を実行する。
3. 成功headerのsession / consumer / captureSequence / captureFlagsが要求と一致することを確認する。
4. ack後だけ新Providerのbutton/keyをImGuiへ渡す。WantCaptureMouseを見た後に遅れて所有を要求しない。
5. 同じflagsの新sequence要求はackとleaseだけ更新し、queue / held状態を破壊しない。
6. GUIを閉じるときはGuestの全pressed状態を解除し、Capture(..., ++request, 0)をackする。
   利用終了時はUnregisterする。tokenが無効なら状態を破棄して再検出・再登録する。

登録だけではlegacy inputを止めない。Mouseは未captureでも観測queueへ入るが、
GuestはownershipなしのBUTTON / WHEELをGUI操作として適用しない。
Keyboard / Textは該当capture中だけqueueへ入る。
Provider未登録時はqueueへ入れず、既存Touch / Keyboard Mappingを維持する。

Host mouse capture中は、同じHost MouseからのVPAD Touchを
InputManagerの取得境界でも抑止する。Keyboard capture中は既存Keyboard Mappingの
state更新とpollの両境界で同じPC Keyを抑止する。SDL等の物理入力経路は変換・停止しない。

TaneClient Phase 3.5.2では次も必須。

- このProviderをMouse sourceにしたGUIでは、既存Touch由来のImGui Mouse event生成を停止する。
  Gamepad Navigationは独立して維持し、ControllerをMouseへ変換しない。
- Providerがない場合だけ既存Touchへfallbackする。backend切替時には全pressed / hoverを解除する。
- Host capture ack前のclickを新Providerへ適用しない。遅れて届いたeventのseqだけから所有を推測しない。
- GUI表示中の所有を先に確立し、通常ゲームへ戻すときはCapture(0)を要求する。

capture開始前からheldだった入力はneutralになるまで新GUIのPressにしない。
capture解除・失効後もheldをlegacyへ新規Pressとして流さず、button/key upを確認して復帰する。
Focus喪失中にupを観測できなかった場合は、次のupまでその入力のlegacy復帰を保留する。
他のkeyは通常入力へ復帰できる。物理button/keyを一度離すことが安全な復帰条件となる。

## Queue、lease、session、復旧

固定queueは256 records / 12,288 bytes、Host Provider全体はMSVC x64で12,960 bytes。
Producer / Consumerは1つのmutexで保護し、Host callbackやRenderer待機をlock内で行わない。
連続した同displayのMOVEだけをcoalesceし、button / wheel / keyの順序をまたがない。
一度のDrainは最大32件。残りがある場合はframe内の予算を決めて追加Drainする。

queue満杯時はcaptureを解除し、queueを明示的なRESET(reason1)へ置き換える。
resetSerialも増加する。releaseを黙って捨ててheld状態を残す契約にはしない。
Guestは次の条件で必ず全Mouse/Keyのpressed、modifier、surrogate、hover状態を解除する。

- RESET record、resetSerialの変化、FOCUS(value0)、CAPTURE_LOST。
- headerのfocus / activeが失われた、session / consumerが変わった、API error。
- GUI非表示、Guest shutdown、backend切替。

resetSerialは**recordを処理する前**に比較する。overflow時の最新captureFlags=0も反映する。
残っているeventが旧所有状態のものならGUIへ適用しない。

leaseはRegister成功時に開始し、Drainまたは成功Captureで更新する。
Queryだけでは更新しない。2秒を超えて更新がなければconsumer / queue / captureを解放する。
expireはHost input / focus / legacy poll / API呼出しで評価され、専用threadや永久blockを作らない。
Guestは毎frame Drainし、focus喪失時も必要ならDrainして状態を取得する。
処理停止や長時間pause後にINVALID_CONSUMERとなったら古いtokenを再利用しない。

Native PLAYごとにsession generationを更新し、STOP / title終了 / Launcher終了では
consumerとqueueを破棄する。終了後に旧sessionの終端recordをDrainできる保証はなく、
active/session照合またはINVALID_CONSUMERを終端通知として扱う。
GuestはGraphic Pack reloadでもConsumerを明示解放し、古いcallbackを残さない。
同一title内で解放できず停止したConsumerもleaseで回復する。
session / consumer / event sequenceがuint32上限に達した場合はwrapして旧値を再使用せずfail-closedとする。

## Guest検証例

次はPPC32用の最小ABI probe。`GuestInputAbi.h`の宣言を使い、
既存coreinit importでIsModuleLoaded / FindExportを解決する前提。
関数addressを推測せず、下記のDynLoad宣言は実際のGuest importへ接続する。
Host componentで関数本体を試験し、例自体はdevkitPPCでcompileだけ行った。
Guest実行、実入力配送、ImGui連携の成功を示すものではない。

```c
#include "input/GuestInputAbi.h"
#if __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
#error This example is for a PPC32 Big Endian guest.
#endif
_Static_assert(sizeof(void*)==4,"PPC32 pointers");
_Static_assert(sizeof(MCWInputHeader)==64 && _Alignof(MCWInputHeader)==4,"header");
_Static_assert(sizeof(MCWInputEvent)==48 && _Alignof(MCWInputEvent)==4,"event");
_Static_assert(offsetof(MCWInputHeader,session)==20 && offsetof(MCWInputHeader,count)==44,"header offsets");
_Static_assert(offsetof(MCWInputEvent,x)==24 && offsetof(MCWInputEvent,code)==32,"event offsets");
extern uint32_t OSDynLoad_IsModuleLoaded(const char*,uint32_t*);
extern uint32_t OSDynLoad_FindExport(uint32_t,uint32_t,const char*,uint32_t*);
typedef uint32_t (*Query)(void*,uint32_t);
typedef uint32_t (*Register)(uint32_t,void*,uint32_t);
typedef uint32_t (*Unregister)(uint32_t,uint32_t);
typedef uint32_t (*Drain)(uint32_t,uint32_t,void*,uint32_t,uint32_t);
typedef uint32_t (*Capture)(uint32_t,uint32_t,uint32_t,uint32_t,void*,uint32_t);
struct Packet { MCWInputHeader header; MCWInputEvent events[MCW_INPUT_MAX_DRAIN]; };
/* 0=fallback, 1=ABI probe passed. No actual user-input acceptance is implied. */
uint32_t GuestInputProbe(void)
{
    uint32_t module=0, address[5]={0};
    static const char* const names[]={"MCWInputQuery","MCWInputRegister","MCWInputUnregister","MCWInputDrain","MCWInputCapture"};
    if (OSDynLoad_IsModuleLoaded(MCW_INPUT_MODULE,&module)!=0 || module==0) return 0;
    for(uint32_t i=0;i<5;++i)
    {
        if(OSDynLoad_FindExport(module,0,names[i],&address[i])!=0 || address[i]==0) return 0;
    }
    Query query=(Query)(uintptr_t)address[0];
    Register registration=(Register)(uintptr_t)address[1];
    Unregister unregistration=(Unregister)(uintptr_t)address[2];
    Drain drain=(Drain)(uintptr_t)address[3];
    Capture capture=(Capture)(uintptr_t)address[4];
    struct Packet packet={0};
    MCWInputHeader* h=&packet.header;
    if(query(h,sizeof(*h))!=0 || h->magic!=MCW_INPUT_MAGIC || (h->version>>16)!=1 ||
       h->headerSize!=64 || h->eventSize!=48 || !h->active || (h->capabilities&31)!=31) return 0;
    if(registration(1,h,sizeof(*h))!=0) return 0;
    const uint32_t session=h->session, consumer=h->consumer;
    uint32_t request=1, success=1;
    if(h->focused)
    {
        if(capture(session,consumer,request,7,h,sizeof(*h))!=0 || h->session!=session ||
           h->consumer!=consumer || h->captureSequence!=request || h->captureFlags!=7) success=0;
        ++request;
    }
    if(drain(session,consumer,&packet,sizeof(packet),MCW_INPUT_MAX_DRAIN)!=0) success=0;
    /* RESET, resetSerial change, Focus Lost or error: release all guest pressed state. */
    if(capture(session,consumer,request,0,h,sizeof(*h))!=0) success=0;
    if(unregistration(session,consumer)!=0) success=0;
    return success;
}
```

製品側では一度のprobeで閉じず、Consumerを適切なlifecycleで保持する。
各frameでDrainし、上記reset/capture契約を適用する。Recordの未知typeはskipする。
Guest側のheaderはこのrepoの定義を基準に必要な宣言だけ導入し、手書きのoffset差を作らない。

## 実際の検証結果と境界

2026-11-27、対象はWindows x64 Native Launcherの開発source。検証レベルは関連入力回帰を含むLevel 2。
ローカル試験と証跡は`.local/tests/guest_input/`、`.local/reports/guest-input/`に保持する。
試験code、ログ、ELF object、EXE、AGENTSは公開対象に含めない。

| 検証 | 結果 | 範囲 |
| --- | --- | --- |
| Hover / L/R/M / edge / drag / vertical+horizontal wheel / leave / capture lost | PASS | Providerと実CemuHost message関数のcomponent fixture |
| Keyboard / modifier / UTF-16 / Unicode scalar / invalid input | PASS | component、実swkbd Unicode回帰もPASS |
| 正規化座標 / corners / letterbox / outside drag / TV-DRC値 | PASS | synthetic viewportの変換と識別。実DRC input sourceは未実装 |
| NativeGameHost同時押し / capture解放 / wheel座標変換 | PASS | 実関数本体、Windows capture/map APIはmock。実UIなし |
| DPI / resize / fullscreenの実Window操作 | NOT RUN | physical pixel契約の静的確認のみ |
| 実module登録 / Query/Register/Drain/Capture/Unregister | PASS | production HLE wrapperをfixture RAM上で実行 |
| module不存在 / optional検出 / 未知export / 無効handle | PASS | 実loader / DynLoad関数本体、依存moduleはfixture |
| BE / sizeof / alignof / offsetof | PASS | Host bytesとdevkitPPC C17/C++20 ELF32 Big Endian compile |
| buffer不正 / bounds / alignment / overflow / read-only | PASS | fake Guest RAM + 実VirtualQuery/VirtualProtect |
| queue / overflow / session / consumer / lease / neutral / capture ack | PASS | deterministic clockとconcurrent producer/consumer |
| Touch / Keyboard二重配送防止、未使用時legacy維持 | PASS | 実CemuHost routing関数 + Provider component |
| 既存Controller / Profile | PASS | 335 synthetic checks、SDL virtual機器、10 reconnect cycles |
| 既存swkbd bridge / Unicode | PASS | 100,000 coalesced変更、世代、terminal、UTF-16/BE、2,000 concurrent invalidate |
| Native Release LauncherShell / CemuBin | PASS | 既存cacheの増分compile/link |
| Standalone LauncherShell | PASS | 既存Standalone cacheの増分compile/link |
| 入力処理performance | PASS | 100,000 MOVE: 未登録92.9 ns/event、登録117.3 ns/event。microbenchmark |
| 不要なevent-loop allocation | PASS | 上記両loopでnew/new[]呼出し0。固定queueの静的確認 |
| F10/F11/Alt+F4維持 | PASS | shortcut分岐の静的確認。実UI操作はNOT RUN |
| 実GamePad / Pro Controller / Steam Input / audio / GPU | NOT RUN | 今回デバイス・実ゲームsessionを起動しない |
| PPC Guest API実行 | NOT RUN | compileとHost componentまで |
| IME composition / clipboard | NOT RUN | 今回の実装対象外 |
| Installed Stable / current.json / settings保護 | PASS | 開始・終了SHA照合。開発EXEの配置・起動なし |
| TaneClient source / Packへの接続 | NOT RUN | Phase 3.5.2。今回変更しない |
| Release / updater / 配布Asset更新 | NOT RUN | 今回行わない |

Computer Use初期化が`Module not found: @oai/sky`で失敗したため、
ユーザー指示どおり環境を改造せず、実UIと実ゲームを起動しなかった。
static/componentのPASSを実Guest入力の成功に置き換えない。

Phase 3.5.1 Implementation: **PASS**。
Phase 3.5.1 Static Acceptance: **PASS**。
Phase 3.5.1 Runtime Acceptance: **PENDING**（PPC Guest APIと実UI受け入れはNOT RUN）。
Phase 3.5.2 Readiness: **PASS**（契約・実装・build・offline検証の引き継ぎ準備。実連携はPENDING）。

## Phase 3.5.2への引き継ぎ

1. Providerのoptional検出、header検査、token保持と破棄をTaneClientの独立backendへ実装する。
2. GUIを開く前のCapture+ack、毎frame Drain、resetSerial、focus、lease復旧を実装する。
3. Q16.16からImGui DisplaySizeへ変換し、BUTTON / WHEEL / KEY / TEXTを標準ImGui APIへ渡す。
   既存Gamepad Navigationを維持し、Touch由来Mouseとの二重配送を止める。
4. 提供Launcherと旧Launcherを分け、hover / text / wheel / first click / focus lost / STOP-PLAY /
   crash lease / viewport / DPI / fullscreen / profile / swkbdの実受け入れを行う。
5. Native buildは既存`out/build/Release/bin/MCWiiU Launcher.exe`。
   開発sourceをbuildしただけでは独立したユーザーデータ環境にならない。
   実行時は既存の分離検証環境を使い、実Windows Known Folder、HKCU、MLC、account、
   input profile、Graphic Packs、WebView data、settingsの保存先が試験用であることを確認する。
   `LOCALAPPDATA`環境変数の差し替えだけで隔離できたと扱わない。
   インストール済みStable、bootstrapperのcurrent.json、署名済み配布物を差し替えない。

今回の完了にユーザーの追加操作は不要。次Phaseの実入力受け入れには、
Computer Useの利用可能な環境と、確認済みの分離Launcher実行環境が必要となる。
