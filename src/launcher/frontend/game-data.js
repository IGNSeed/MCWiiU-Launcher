"use strict";

window.GameDataUI = (() => {
  let snapshot = null, serial = Date.now() * 1000, pending = null, deferredCommand = null, lastPoll = 0;
  let mode = "", steps = [], step = 0, watchedOperation = "", renderingKey = "", confirmationAction = null;
  const $ = id => document.getElementById(id);
  const stage = $("setup-stage"), notice = $("setup-notice"), confirmation = $("game-data-confirmation");
  const labels = {base:"BASE",get update() { return LauncherI18n.t("Update v560"); },dlc:"DLC",get review() { return LauncherI18n.t("Review"); },get progress() { return LauncherI18n.t("Install"); },get complete() { return LauncherI18n.t("Complete"); }};
  const phaseLabels = {get Idle() { return LauncherI18n.t("Waiting"); },get Analyzing() { return LauncherI18n.t("Analyzing"); },get Planning() { return LauncherI18n.t("Checking space and operation plan"); },get Copying() { return LauncherI18n.t("Copying"); },get Placing() { return LauncherI18n.t("Committing"); },get RollingBack() { return LauncherI18n.t("Restoring previous state"); },get Cleaning() { return LauncherI18n.t("Cleaning up"); },get Completed() { return LauncherI18n.t("Finished"); }};
  const errors = {get Busy() { return LauncherI18n.t("Another operation is running. Wait for it and pending saves to finish."); },get GameActive() { return LauncherI18n.t("Stop the game before continuing."); },get InvalidPath() { return LauncherI18n.t("Select a title folder or meta/meta.xml."); },get UnsafePath() { return LauncherI18n.t("Unsafe path. Links and data inside managed storage cannot be selected."); },get InvalidMetadata() { return LauncherI18n.t("Check metadata, title ID and the selection session."); },get UnsupportedTitle() { return LauncherI18n.t("Select the requested Minecraft title type."); },get UnsupportedVersion() { return LauncherI18n.t("Only Update v560 is supported."); },get RegionMismatch() { return LauncherI18n.t("BASE and Update must use the same region."); },get AlreadyInstalled() { return LauncherI18n.t("Already installed. Use Reinstall."); },get NotInstalled() { return LauncherI18n.t("A required title is missing."); },get UnmanagedTitle() { return LauncherI18n.t("Existing unmanaged titles were preserved."); },get NoSpace() { return LauncherI18n.t("Not enough free space."); },get AccessDenied() { return LauncherI18n.t("Access denied."); },get CopyFailed() { return LauncherI18n.t("Copy failed."); },get Cancelled() { return LauncherI18n.t("Operation cancelled."); },get InvalidJournal() { return LauncherI18n.t("Invalid operation journal. Preserve it for recovery."); },get RecoveryRequired() { return LauncherI18n.t("Recovery required. Preserve existing data and journals, close the launcher and review the journal."); },get IO() { return LauncherI18n.t("File operation failed."); }};
  window.addEventListener("launcher-language-changed", () => { renderingKey = ""; renderData(); renderSetup(); });
  function errorText(error, message) { return error && error !== "None" ? (errors[error] || LauncherI18n.t("Could not complete the action.")) + (message ? "\n" + message : "") : ""; }
  function element(tag, text, className) { const node = document.createElement(tag); if (text !== undefined) LauncherI18n.bind(node, "textContent", () => text); if (className) node.className = className; return node; }
  function button(text, action, disabled = false) { const node = element("button", text); node.type = "button"; node.disabled = disabled; node.addEventListener("click", action); return node; }
  function titleText(title) { return title ? `${title.name || title.id} · ${title.region} · v${title.version}\n${title.id}` : LauncherI18n.t("Not installed"); }
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
    LauncherI18n.bind($("game-data-confirm-title"), "textContent", () => title); LauncherI18n.bind($("game-data-confirm-text"), "textContent", () => text);
    LauncherI18n.bind($("game-data-confirm-accept"), "textContent", () => accept); confirmationAction = action; confirmation.showModal();
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
    step = 0; LauncherI18n.bind(notice, "textContent", () => "");
    send("begin", {operation,...extra});
  }
  function start() {
    if (!editable() || !snapshot?.scope) return;
    send("start", {scope:snapshot.scope,selectionIds:snapshot.choices.map(value => value.selectionId)});
  }
  function startReviewed() {
    if (mode === "Setup" || mode === "InstallUpdate" || mode === "InstallDLC") { start(); return; }
    const target = mode === "RemoveDLC" ? snapshot.dlcTitleId : mode === "Uninstall" ? LauncherI18n.t("BASE · Update · DLC") : mode === "ChangeRegion" ? `${snapshot.region} → ${snapshot.targetRegion}` : labels[mode === "ReinstallBase" ? "base" : "update"];
    ask(mode, LauncherI18n.t("{0}\n\n{1}\nWorlds, saves, accounts, Mii, settings, controller profiles, graphic packs and textures are preserved.\nSaves are not converted or copied between regions.", [target, mode === "Uninstall" || mode === "RemoveDLC" ? LauncherI18n.t("Selected game title data will be deleted.") : LauncherI18n.t("Game title data will be changed as shown.")]), mode === "Uninstall" || mode === "RemoveDLC" ? LauncherI18n.t("Delete") : LauncherI18n.t("Continue"), start);
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
    LauncherI18n.bind($("setup-title"), "textContent", () => mode === "Setup" ? LauncherI18n.t("Minecraft Wii U Setup") : mode === "ChangeRegion" ? LauncherI18n.t("Change Region") : mode === "Uninstall" ? LauncherI18n.t("Uninstall Game Data") : mode === "RemoveDLC" ? LauncherI18n.t("Remove DLC") : mode.replace(/([a-z])([A-Z])/g,"$1 $2"));
    const current = steps[step];
    LauncherI18n.bind($("setup-step"), "textContent", () => LauncherI18n.t("STEP {0} / {1} · {2}", [step + 1, steps.length, labels[current]]));
    if (renderingKey !== key) {
      const restoreCancelFocus = document.activeElement?.id === "setup-cancel-operation";
      renderingKey = key; stage.replaceChildren();
      if (["base","update","dlc"].includes(current)) {
        stage.append(element("h2",labels[current],"settings-heading"));
        stage.append(element("p",current === "dlc" ? LauncherI18n.t("DLC is optional. Other regions are preserved. Cross-region DLC mounting is not guaranteed.") : LauncherI18n.t("Select a title folder containing code / content / meta, or meta/meta.xml."), "footnote"));
        const selected = usesInstalled(current) ? installed(current) : choice(current)?.title;
        stage.append(element("p",selected ? titleText(selected) + (usesInstalled(current) ? LauncherI18n.t("\nInstalled · Preserved") : "\nSelected") : LauncherI18n.t("Not selected"), "game-data-title"));
        if (!usesInstalled(current)) {
          const actions = element("div",undefined,"game-data-actions");
          actions.append(button(LauncherI18n.t("Select Folder"), () => pick(current,true), !editable()),button(LauncherI18n.t("Select meta.xml"), () => pick(current,false), !editable()));
          if (current === "dlc") actions.append(button(LauncherI18n.t("Skip"), () => { if (choice("dlc")) send("clear",{scope:snapshot.scope,part:"dlc"}); step++; renderingKey=""; renderSetup(); }, !editable()));
          stage.append(actions);
        }
        if (snapshot.selectionPending) stage.append(element("p",LauncherI18n.t("Analyzing selected source…"), "footnote"));
      } else if (current === "review") {
        const list = element("dl",undefined,"settings-info game-data-review");
        for (const part of ["base","update","dlc"]) {
          const selected = choice(part)?.title, retained = snapshot.titles.filter(title => title.type === part);
          list.append(element("dt",labels[part]),element("dd",selected ? titleText(selected) : retained.length ? retained.map(title => titleText(title) + (mode === "Uninstall" || (mode === "RemoveDLC" && title.id === snapshot.dlcTitleId) ? LauncherI18n.t("\nWill be deleted") : LauncherI18n.t("\nPreserved"))).join("\n") : LauncherI18n.t("Not selected")));
        }
        list.append(element("dt",LauncherI18n.t("Region")),element("dd",snapshot.targetRegion || choice("base")?.title.region || snapshot.region),element("dt","Installation"),element("dd",LauncherI18n.t("MCWiiU-Launcher LocalAppData"))); stage.append(list);
        stage.append(element("p",LauncherI18n.t("Worlds, saves and settings are preserved. The game will not launch automatically."), "footnote"));
      } else if (current === "progress") {
        const p = snapshot.progress;
        stage.append(element("h2",phaseLabels[p.phase] || p.phase,"settings-heading"),element("p",p.titleId === "0000000000000000" ? mode : p.titleId,"game-data-title"));
        const bar = element("progress"); bar.max = 100; bar.value = Math.max(0,Math.min(100,p.percent)); LauncherI18n.bind(bar, "aria-label", () => LauncherI18n.t("Copy progress"), true);
        stage.append(bar,element("p",`${p.percent.toFixed(1)}% · ${bytes(p.processedBytes)} / ${bytes(p.totalBytes)}`,"footnote"));
        stage.append(element("p",p.cancellable ? LauncherI18n.t("You may cancel or browse other pages while this operation continues.") : LauncherI18n.t("Wait for committing, recovery and cleanup to finish. Cancellation is unavailable at this stage."), "footnote"));
        const cancelButton = button(LauncherI18n.t("Cancel"), () => ask(LauncherI18n.t("Cancel installation"), LauncherI18n.t("Cancel installation? Incomplete data will be handled safely."), LauncherI18n.t("Cancel installation"), () => send("cancel",{scope:watchedOperation})), !p.cancellable || !!pending || closeAfterSaves);
        cancelButton.id = "setup-cancel-operation"; stage.append(cancelButton);
        if (restoreCancelFocus) cancelButton.focus({preventScroll:true});
      } else {
        const p = snapshot.progress, success = p.completion === "Succeeded";
        stage.append(element("h2",success ? (mode === "Setup" ? LauncherI18n.t("Installation Complete") : LauncherI18n.t("Operation Complete")) : p.completion === "Cancelled" ? "Cancelled" : LauncherI18n.t("Operation Failed"),"settings-heading"));
        stage.append(element("p",success ? (snapshot.ready ? `BASE + Update v560 · ${snapshot.region}\n${snapshot.launchSupported ? LauncherI18n.t("The game is ready.") : LauncherI18n.t("Check the game data status.")}` : LauncherI18n.t("Game data status refreshed.")) : errorText(p.error,p.message),"game-data-title"));
        stage.append(element("p",snapshot.state === "RecoveryRequired" ? errors.RecoveryRequired : p.completion === "Cancelled" ? LauncherI18n.t("Cancellation completed. See Game Data for the current installation status.") : LauncherI18n.t("Current status: ") + snapshot.state,"footnote"));
      }
    }
    renderControls();
  }
  function renderControls() {
    const current = steps[step], locked = !editable() || (!snapshot?.scope && current !== "progress" && current !== "complete");
    $("setup-back").hidden = step === 0 || current === "progress" || current === "complete";
    $("setup-back").disabled = locked;
    $("setup-next").hidden = current === "progress" || current === "complete";
    LauncherI18n.bind($("setup-next"), "textContent", () => current === "review" ? LauncherI18n.t("Continue") : LauncherI18n.t("Next"));
    $("setup-next").disabled = locked || (["base","update"].includes(current) && !usesInstalled(current) && !choice(current));
    $("setup-leave").disabled = !!pending || snapshot?.selectionPending || current === "progress";
    LauncherI18n.bind($("setup-leave"), "textContent", () => current === "complete" ? (mode === "Setup" ? LauncherI18n.t("Back to Home") : LauncherI18n.t("Back to Game Data")) : LauncherI18n.t("Cancel"));
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
  const installationLabels = {NotInstalled:"Not installed", BaseOnly:"BASE only", UpdateMissing:"Update required", Ready:"Ready", Installing:"Installing", Reinstalling:"Reinstalling", ChangingRegion:"Changing region", Uninstalling:"Uninstalling", RecoveryRequired:"Recovery required", Error:"Error"};
  function renderData() {
    if (!snapshot) return;
    const list = $("game-data-summary"); list.replaceChildren();
    const missing = snapshot.state === "RecoveryRequired" ? LauncherI18n.t("Recovery required · State unconfirmed") : LauncherI18n.t("Not installed");
    for (const [name,value] of [[LauncherI18n.t("Region"),snapshot.region],[LauncherI18n.t("Status"),LauncherI18n.t(installationLabels[snapshot.state] || snapshot.state)],["BASE",snapshot.titles.filter(t=>t.type==="base").map(titleText).join("\n") || missing],["Update",snapshot.titles.filter(t=>t.type==="update").map(titleText).join("\n") || missing],["DLC",snapshot.titles.filter(t=>t.type==="dlc").map(titleText).join("\n") || missing]]) list.append(element("dt",name),element("dd",value));
    const select = $("game-data-dlc"), previous = select.value;
    const key = snapshot.titles.filter(t=>t.type==="dlc").map(t=>t.id).join();
    if (select.dataset.titles !== key) { select.dataset.titles = key; select.replaceChildren(); for (const title of snapshot.titles.filter(t=>t.type==="dlc")) { const option = element("option",`${title.region} · ${title.id}`); option.value = title.id; select.append(option); } if ([...select.options].some(o=>o.value===previous)) select.value = previous; }
    LauncherI18n.bind($("game-data-region-note"), "textContent", () => snapshot.ready && !snapshot.launchSupported ? LauncherI18n.t("Game data unavailable. Check the status and recovery instructions.") : !snapshot.nativeAvailable ? LauncherI18n.t("Standalone has no Cemu Core. Game data can be managed, but games cannot launch.") : "");
    const stateError = errorText(snapshot.stateError,snapshot.stateMessage) || errorText(snapshot.error,snapshot.message);
    LauncherI18n.bind($("game-data-notice"), "textContent", () => snapshot.state === "RecoveryRequired" ? [errors.RecoveryRequired,stateError].filter(Boolean).join("\n") :
      stateError || (!snapshot.canManage ? LauncherI18n.t("Available when the game and pending operations have stopped and Core and settings are healthy.") : ""));
    $("game-data-banner").hidden = snapshot.progress.completion !== "Running";
    LauncherI18n.bind($("game-data-banner-text"), "textContent", () => phaseLabels[snapshot.progress.phase] || snapshot.progress.phase);
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
    LauncherI18n.bind(notice, "textContent", () => errorText(value.error,value.message));
    if (!watchedOperation && !steps.length && value.progress.completion === "Running" && value.operationId) {
      watchedOperation = value.operationId; mode = value.mode; steps = ["progress","complete"]; step = 0;
    }
    if (op === "begin" && value.error === "None") showSetup();
    if (op === "start" && value.error === "None") { watchedOperation = value.operationId; step = steps.indexOf("progress"); showSetup(); }
    if (watchedOperation && value.operationId === watchedOperation && ["Succeeded","Failed","Cancelled"].includes(value.progress.completion)) step = steps.indexOf("complete");
    renderData(); renderSetup(); updatePlay();
    LauncherI18n.bind($("home-game-data-note"), "textContent", () => value.state === "RecoveryRequired" ? errors.RecoveryRequired : value.ready && !value.launchSupported ? LauncherI18n.t("Check the game data status and recovery instructions.") : !value.nativeAvailable ? LauncherI18n.t("Standalone · Game launch unavailable. Use PLAY to set up game data.") : !value.ready ? (value.canSetup ? LauncherI18n.t("Use PLAY to set up Minecraft BASE and Update v560.") : LauncherI18n.t("Check Core, settings and pending operations. Setup becomes available through PLAY when ready.")) : "");
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
      ask(LauncherI18n.t("Exit launcher"), LauncherI18n.t("A game data operation is running. Cancellable work will stop safely; other stages must finish before exit."), LauncherI18n.t("Exit"), finish);
    } else finish();
  }};
})();
