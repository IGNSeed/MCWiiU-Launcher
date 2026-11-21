"use strict";

window.GameDataUI = (() => {
  let snapshot = null, serial = Date.now() * 1000, pending = null, deferredCommand = null, lastPoll = 0;
  let mode = "", steps = [], step = 0, watchedOperation = "", renderingKey = "", confirmationAction = null;
  const $ = id => document.getElementById(id);
  const stage = $("setup-stage"), notice = $("setup-notice"), confirmation = $("game-data-confirmation");
  const labels = {base:"BASE",update:"Update v560",dlc:"DLC",review:"内容確認",progress:"インストール",complete:"完了"};
  const phaseLabels = {Idle:"待機中",Analyzing:"解析中",Planning:"容量・操作計画を確認中",Copying:"コピー中",Placing:"正式配置中",RollingBack:"変更前の状態へ復旧中",Cleaning:"後処理中",Completed:"処理終了"};
  const errors = {Busy:"別の操作が実行中です。停止・保存の完了後に再試行してください。",GameActive:"ゲームを停止してから操作してください。",InvalidPath:"タイトルフォルダまたはmeta/meta.xmlを選択してください。",UnsafePath:"安全に利用できないパスです。リンクや管理ストレージ内のデータは選択できません。",InvalidMetadata:"メタデータ、タイトルID、または選択セッションを確認してください。",UnsupportedTitle:"指定されたMinecraftのタイトル種別を選択してください。",UnsupportedVersion:"Updateはv560のみ利用できます。",RegionMismatch:"BASEとUpdateの地域を一致させてください。",AlreadyInstalled:"既にインストールされています。再インストール操作を使用してください。",NotInstalled:"操作に必要なタイトルがありません。",UnmanagedTitle:"管理対象外の既存タイトルを保持しました。",NoSpace:"空き容量が不足しています。",AccessDenied:"アクセスが拒否されました。",CopyFailed:"コピーに失敗しました。",Cancelled:"操作をキャンセルしました。",InvalidJournal:"不正な操作記録があります。削除せずに保全してください。",RecoveryRequired:"復旧が必要です。既存データと操作記録を削除せず保全し、Launcherを終了して記録を確認してください。",IO:"ファイル操作に失敗しました。"};
  function errorText(error, message) { return error && error !== "None" ? (errors[error] || "操作できませんでした。") + (message ? "\n" + message : "") : ""; }
  function element(tag, text, className) { const node = document.createElement(tag); if (text !== undefined) node.textContent = text; if (className) node.className = className; return node; }
  function button(text, action, disabled = false) { const node = element("button", text); node.type = "button"; node.disabled = disabled; node.addEventListener("click", action); return node; }
  function titleText(title) { return title ? `${title.name || title.id} · ${title.region} · v${title.version}\n${title.id}` : "Not installed"; }
  function installed(part) { return snapshot?.titles.find(title => title.type === part && (part !== "update" || title.version === 560)); }
  function choice(part) { return snapshot?.choices.find(value => value.part === part); }
  function usesInstalled(part) { return mode === "Setup" && !!installed(part) && part !== "dlc"; }
  function editable() { return snapshot?.canManage === true && !deferredCommand && (!pending || pending.op === "get") && !closeAfterSaves && !settingsBusy(); }
  function canSetup() { return snapshot?.canSetup === true && editable(); }
  function send(op, extra = {}) {
    if (pending) {
      if (pending.op === "get" && op !== "get" && !deferredCommand) {
        deferredCommand = {op,extra}; renderControls(); updatePlay(); return true;
      }
      return false;
    }
    pending = {requestId:++serial,op}; lastPoll = performance.now();
    windowCommand(`game-data:command:${pending.requestId}:` + JSON.stringify({op,...extra}));
    renderControls(); updatePlay(); return true;
  }
  function ask(title, text, accept, action) {
    if (confirmation.open) return;
    $("game-data-confirm-title").textContent = title; $("game-data-confirm-text").textContent = text;
    $("game-data-confirm-accept").textContent = accept; confirmationAction = action; confirmation.showModal();
  }
  function cancelConfirmation() {
    confirmationAction = null; confirmation.close();
    if (confirmation.dataset.closing) { delete confirmation.dataset.closing; windowCommand("window:close-cancelled"); }
  }
  $("game-data-confirm-cancel").addEventListener("click", cancelConfirmation);
  confirmation.addEventListener("cancel", event => { event.preventDefault(); cancelConfirmation(); });
  $("game-data-confirm-accept").addEventListener("click", () => { const action = confirmationAction; confirmationAction = null; delete confirmation.dataset.closing; confirmation.close(); action?.(); });
  function showSetup() {
    showPage("setup");
    renderingKey = ""; renderSetup();
  }
  function begin(operation, extra = {}) {
    if (!editable()) return;
    mode = operation; watchedOperation = "";
    steps = operation === "Setup" ? ["base","update","dlc","review","progress","complete"] :
      operation === "ChangeRegion" ? ["base","update","review","progress","complete"] :
      [operation === "ReinstallBase" ? "base" : operation === "InstallDLC" ? "dlc" : "update","review","progress","complete"];
    if (operation === "Uninstall" || operation === "RemoveDLC") steps = ["review","progress","complete"];
    step = 0; notice.textContent = "";
    send("begin", {operation,...extra});
  }
  function start() {
    if (!editable() || !snapshot?.scope) return;
    send("start", {scope:snapshot.scope,selectionIds:snapshot.choices.map(value => value.selectionId)});
  }
  function startReviewed() {
    if (mode === "Setup" || mode === "InstallUpdate" || mode === "InstallDLC") { start(); return; }
    const target = mode === "RemoveDLC" ? snapshot.dlcTitleId : mode === "Uninstall" ? "BASE・Update・DLC" : mode === "ChangeRegion" ? `${snapshot.region} → ${snapshot.targetRegion}` : labels[mode === "ReinstallBase" ? "base" : "update"];
    ask(mode, `${target}\n\n${mode === "Uninstall" || mode === "RemoveDLC" ? "対象のゲームタイトルデータを削除します。" : "表示した内容でゲームタイトルデータを変更します。"}\nワールド・セーブ・Account・Mii・設定・Controller Profiles・Graphic Packs・Texturesは保持されます。\n地域間のセーブ変換・コピーは行いません。`, mode === "Uninstall" || mode === "RemoveDLC" ? "削除する" : "実行する", start);
  }
  function pick(part, folder) { if (editable() && snapshot.scope) send(folder ? "pick-folder" : "pick-meta", {scope:snapshot.scope,part}); }
  function leave() {
    if (steps[step] === "progress" || snapshot?.selectionPending || pending) return;
    if (snapshot?.scope) send("discard", {scope:snapshot.scope});
    showPage(mode === "Setup" ? "home" : "settings");
    if (mode !== "Setup") showTab("game-data");
  }
  function renderSetup() {
    if (!snapshot || !steps.length) return;
    const key = JSON.stringify([mode,step,snapshot.choices,snapshot.titles,snapshot.selectionPending,snapshot.state,steps[step] === "progress" || steps[step] === "complete" ? snapshot.progress : null,snapshot.canManage,!!pending && pending.op !== "get"]);
    $("setup-title").textContent = mode === "Setup" ? "Minecraft Wii U Setup" : mode === "ChangeRegion" ? "Change Region" : mode === "Uninstall" ? "Uninstall Game Data" : mode === "RemoveDLC" ? "Remove DLC" : mode.replace(/([a-z])([A-Z])/g,"$1 $2");
    const current = steps[step];
    $("setup-step").textContent = `STEP ${step + 1} / ${steps.length} · ${labels[current]}`;
    if (renderingKey !== key) {
      const restoreCancelFocus = document.activeElement?.id === "setup-cancel-operation";
      renderingKey = key; stage.replaceChildren();
      if (["base","update","dlc"].includes(current)) {
        stage.append(element("h2",labels[current],"settings-heading"));
        stage.append(element("p",current === "dlc" ? "DLCは任意です。地域の異なるDLCも保持します。Coreでの地域間マウント互換性は保証しません。" : "code / content / metaを含むタイトルフォルダ、またはmeta/meta.xmlを選択してください。", "footnote"));
        const selected = usesInstalled(current) ? installed(current) : choice(current)?.title;
        stage.append(element("p",selected ? titleText(selected) + (usesInstalled(current) ? "\nInstalled · 保持します" : "\nSelected") : "Not selected", "game-data-title"));
        if (!usesInstalled(current)) {
          const actions = element("div",undefined,"game-data-actions");
          actions.append(button("フォルダを選択", () => pick(current,true), !editable()),button("meta.xmlを選択", () => pick(current,false), !editable()));
          if (current === "dlc") actions.append(button("スキップ", () => { if (choice("dlc")) send("clear",{scope:snapshot.scope,part:"dlc"}); step++; renderingKey=""; renderSetup(); }, !editable()));
          stage.append(actions);
        }
        if (snapshot.selectionPending) stage.append(element("p","選択したソースを解析中…", "footnote"));
      } else if (current === "review") {
        const list = element("dl",undefined,"settings-info game-data-review");
        for (const part of ["base","update","dlc"]) {
          const selected = choice(part)?.title, retained = snapshot.titles.filter(title => title.type === part);
          list.append(element("dt",labels[part]),element("dd",selected ? titleText(selected) : retained.length ? retained.map(title => titleText(title) + (mode === "Uninstall" || (mode === "RemoveDLC" && title.id === snapshot.dlcTitleId) ? "\n削除対象" : "\n保持します")).join("\n") : "Not selected"));
        }
        list.append(element("dt","Region"),element("dd",snapshot.targetRegion || choice("base")?.title.region || snapshot.region),element("dt","Installation"),element("dd","MCWiiU-Launcher LocalAppData")); stage.append(list);
        stage.append(element("p","ワールド・セーブ・設定は保持します。インストール完了後、ゲームは自動起動しません。", "footnote"));
      } else if (current === "progress") {
        const p = snapshot.progress;
        stage.append(element("h2",phaseLabels[p.phase] || p.phase,"settings-heading"),element("p",p.titleId === "0000000000000000" ? mode : p.titleId,"game-data-title"));
        const bar = element("progress"); bar.max = 100; bar.value = Math.max(0,Math.min(100,p.percent)); bar.setAttribute("aria-label","コピー進捗");
        stage.append(bar,element("p",`${p.percent.toFixed(1)}% · ${bytes(p.processedBytes)} / ${bytes(p.totalBytes)}`,"footnote"));
        stage.append(element("p",p.cancellable ? "キャンセルできます。別の画面へ移動しても処理は継続します。" : "正式配置・復旧・後処理が終わるまでお待ちください。この段階ではキャンセルできません。", "footnote"));
        const cancelButton = button("キャンセル", () => ask("インストールを中止", "インストールを中止しますか？未完了のデータは安全に処理されます。", "中止する", () => send("cancel",{scope:watchedOperation})), !p.cancellable || !!pending || closeAfterSaves);
        cancelButton.id = "setup-cancel-operation"; stage.append(cancelButton);
        if (restoreCancelFocus) cancelButton.focus({preventScroll:true});
      } else {
        const p = snapshot.progress, success = p.completion === "Succeeded";
        stage.append(element("h2",success ? (mode === "Setup" ? "Installation Complete" : "Operation Complete") : p.completion === "Cancelled" ? "Cancelled" : "Operation Failed","settings-heading"));
        stage.append(element("p",success ? (snapshot.ready ? `BASE + Update v560 · ${snapshot.region}\n${snapshot.launchSupported ? "The game is ready." : "ゲームデータの状態を確認してください。"}` : "ゲームデータの状態を更新しました。") : errorText(p.error,p.message),"game-data-title"));
        stage.append(element("p",snapshot.state === "RecoveryRequired" ? errors.RecoveryRequired : p.completion === "Cancelled" ? "バックエンドがキャンセル処理を完了しました。現在のインストール状態はGame Dataで確認できます。" : "現在の状態: " + snapshot.state,"footnote"));
      }
    }
    renderControls();
  }
  function renderControls() {
    const current = steps[step], locked = !editable() || (!snapshot?.scope && current !== "progress" && current !== "complete");
    $("setup-back").hidden = step === 0 || current === "progress" || current === "complete";
    $("setup-back").disabled = locked;
    $("setup-next").hidden = current === "progress" || current === "complete";
    $("setup-next").textContent = current === "review" ? "実行する" : "次へ";
    $("setup-next").disabled = locked || (["base","update"].includes(current) && !usesInstalled(current) && !choice(current));
    $("setup-leave").disabled = !!pending || snapshot?.selectionPending || current === "progress";
    $("setup-leave").textContent = current === "complete" ? (mode === "Setup" ? "Back to Home" : "Back to Game Data") : "キャンセル";
    for (const node of document.querySelectorAll("[data-game-operation]")) node.disabled = !editable() || !allowed(node.dataset.gameOperation);
    $("game-data-region").disabled = !editable(); $("game-data-dlc").disabled = !editable();
  }
  function allowed(operation) {
    if (!snapshot) return false;
    const has = part => snapshot.titles.some(title => title.type === part);
    return operation === "Setup" ? !snapshot.ready : operation === "InstallUpdate" ? has("base") && !has("update") :
      operation === "InstallDLC" ? true : operation === "ReinstallBase" ? has("base") : operation === "ReinstallUpdate" ? has("update") :
      operation === "ChangeRegion" ? has("base") && $("game-data-region").value !== snapshot.region : operation === "RemoveDLC" ? has("dlc") : operation === "Uninstall" ? !!snapshot.titles.length : false;
  }
  function bytes(value) { return value >= 1073741824 ? (value / 1073741824).toFixed(2) + " GiB" : value >= 1048576 ? (value / 1048576).toFixed(1) + " MiB" : value + " B"; }
  function renderData() {
    if (!snapshot) return;
    const list = $("game-data-summary"); list.replaceChildren();
    const missing = snapshot.state === "RecoveryRequired" ? "Recovery required · 状態を確定できません" : "Not installed";
    for (const [name,value] of [["Region",snapshot.region],["Status",snapshot.state],["BASE",snapshot.titles.filter(t=>t.type==="base").map(titleText).join("\n") || missing],["Update",snapshot.titles.filter(t=>t.type==="update").map(titleText).join("\n") || missing],["DLC",snapshot.titles.filter(t=>t.type==="dlc").map(titleText).join("\n") || missing]]) list.append(element("dt",name),element("dd",value));
    const select = $("game-data-dlc"), previous = select.value;
    const key = snapshot.titles.filter(t=>t.type==="dlc").map(t=>t.id).join();
    if (select.dataset.titles !== key) { select.dataset.titles = key; select.replaceChildren(); for (const title of snapshot.titles.filter(t=>t.type==="dlc")) { const option = element("option",`${title.region} · ${title.id}`); option.value = title.id; select.append(option); } if ([...select.options].some(o=>o.value===previous)) select.value = previous; }
    $("game-data-region-note").textContent = snapshot.ready && !snapshot.launchSupported ? "ゲームデータを確認できません。状態と復旧案内を確認してください。" : !snapshot.nativeAvailable ? "StandaloneにはCemu Coreがありません。データ管理は利用できますが、ゲームは起動できません。" : "";
    const stateError = errorText(snapshot.stateError,snapshot.stateMessage) || errorText(snapshot.error,snapshot.message);
    $("game-data-notice").textContent = snapshot.state === "RecoveryRequired" ? [errors.RecoveryRequired,stateError].filter(Boolean).join("\n") :
      stateError || (!snapshot.canManage ? "ゲーム・保存・管理操作が停止し、Coreと設定が正常な状態で変更できます。" : "DLCは任意です。地域間でのCoreのDLCマウント互換性は保証しません。");
    $("game-data-banner").hidden = snapshot.progress.completion !== "Running";
    $("game-data-banner-text").textContent = phaseLabels[snapshot.progress.phase] || snapshot.progress.phase;
    $("game-data-view-progress").hidden = !watchedOperation;
    renderControls();
  }
  $("setup-back").addEventListener("click", () => { if (editable() && step > 0) { step--; renderingKey=""; renderSetup(); } });
  $("setup-next").addEventListener("click", () => { if ($("setup-next").disabled) return; if (steps[step] === "review") startReviewed(); else { step++; renderingKey=""; renderSetup(); } });
  $("setup-leave").addEventListener("click", leave);
  $("game-data-view-progress").addEventListener("click", showSetup);
  $("game-data-region").addEventListener("change",renderControls);
  for (const node of document.querySelectorAll("[data-game-operation]")) node.addEventListener("click", () => begin(node.dataset.gameOperation,{targetRegion:$("game-data-region").value,dlcTitleId:$("game-data-dlc").value}));
  window.addEventListener("launcher-game-data", event => {
    const value = event.detail;
    if (!pending || value.requestId !== pending.requestId) return;
    const op = pending.op; pending = null; snapshot = value; window.gameDataSnapshot = value;
    notice.textContent = errorText(value.error,value.message);
    if (!watchedOperation && !steps.length && value.progress.completion === "Running" && value.operationId) {
      watchedOperation = value.operationId; mode = value.mode; steps = ["progress","complete"]; step = 0;
    }
    if (op === "begin" && value.error === "None") showSetup();
    if (op === "start" && value.error === "None") { watchedOperation = value.operationId; step = steps.indexOf("progress"); showSetup(); }
    if (watchedOperation && value.operationId === watchedOperation && ["Succeeded","Failed","Cancelled"].includes(value.progress.completion)) step = steps.indexOf("complete");
    renderData(); renderSetup(); updatePlay();
    $("home-game-data-note").textContent = value.state === "RecoveryRequired" ? errors.RecoveryRequired : value.ready && !value.launchSupported ? "ゲームデータの状態と復旧案内を確認してください。" : !value.nativeAvailable ? "Standalone · ゲーム起動機能なし。PLAYからデータのセットアップを行えます。" : !value.ready ? (value.canSetup ? "PLAYからMinecraft BASEとUpdate v560をセットアップできます。" : "Core・設定・実行中の処理を確認してください。操作可能になるとPLAYからセットアップできます。") : "";
    if (closeAfterSaves && !value.busy) drainSaves();
    if (deferredCommand) {
      const command = deferredCommand; deferredCommand = null;
      if (!closeAfterSaves) send(command.op,command.extra);
    }
  });
  window.addEventListener("launcher-core-state", () => { renderControls(); updatePlay(); });
  setInterval(() => {
    const interval = snapshot?.busy || snapshot?.scope || closeAfterSaves || (selectedPage === "settings" && selectedTab === "game-data") ? 500 : 2000;
    if (!pending && performance.now()-lastPoll >= interval) send("get");
  }, 250);
  send("get");
  return {canSetup,onPlay:() => begin("Setup"),busy:() => !!deferredCommand || (!!pending && pending.op !== "get") || snapshot?.busy === true,beforeClose(event,finish) {
    if (event.detail?.gameDataBusy || snapshot?.busy) {
      confirmation.dataset.closing = "true";
      ask("Launcherを終了", "ゲームデータの操作中です。キャンセル可能な処理は安全に中止し、配置・復旧・解析中は完了を待って終了します。", "終了する", finish);
    } else finish();
  }};
})();
