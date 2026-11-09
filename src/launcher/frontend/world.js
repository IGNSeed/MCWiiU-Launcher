"use strict";

const worldList = document.getElementById("world-list"), worldDialog = document.getElementById("world-dialog");
const worldName = document.getElementById("world-name"), worldConfirm = document.getElementById("world-confirm"), worldDestination = document.getElementById("world-destination");
let worldSnapshot = null, worldSerial = Date.now(), worldPending = 0, worldPendingOp = "", worldTarget = null, worldFocus = null, worldVisible = false;
const worldNotices = {
  ready:"", saved:"保存しました。", unchanged:"変更はありません。", cancelled:"画像の選択を取り消しました。", "preview-ready":"プレビューを確認して Save を押してください。",
  "not-ready":"ゲームと終了処理が停止し、準備完了になってから操作してください。", "stale-request":"アカウントまたは一覧が変わりました。Refresh して操作し直してください。",
  "stale-world":"ワールドが更新されました。Refresh して操作し直してください。", "invalid-name":"名前は1〜32 UTF-16単位で入力してください。制御文字や空白だけの名前は使えません。",
  "save-id-conflict":"同じSave IDが移動先に存在します。上書きせず中止しました。", "invalid-image":"画像を読み込めません。16 MB以下のPNG / JPEGを選択してください。",
  "unsupported-metadata":"この保存形式は安全に編集できません。", "invalid-world":"このワールドを安全に読み込めません。", "invalid-account":"アカウントを確認できません。",
  "unsafe-path":"安全な保存先を確認できません。", "stale-preview":"画像プレビューが古くなりました。画像を選び直してください。",
  "recovery-required":"中断した操作があります。復旧が完了するまでゲーム起動と編集を停止しています。",
  recovered:"変更前のワールドを復旧しました。", "rolled-back":"操作に失敗したため、変更前に復旧しました。", "read-failed":"ファイルを読み込めません。ほかのアプリで使用中の場合は閉じてください。",
  "write-failed":"書き込みに失敗しました。保存先の空き容量や使用状況を確認してください。", "verification-failed":"書き込み後の整合性を確認できませんでした。",
  "invalid-file":"ファイルの形式やサイズを確認できません。", "world-limit":"ワールド数が上限を超えています。", "invalid-request":"操作を検証できません。Refresh してください。"
};
function worldMessage(notice) { return worldNotices[notice] ?? "操作を完了できませんでした。"; }
function worldNode(tag, text, className) {
  const node = document.createElement(tag);
  if (text !== undefined) node.textContent = text;
  if (className) node.className = className;
  return node;
}
function validWorldName(name) {
  return name.length > 0 && name.length <= 32 && name.isWellFormed() && !/[\u0000-\u001f\u007f]/u.test(name) && !/^[ \u3000]*$/u.test(name);
}
function worldCanEdit() { return worldSnapshot?.canEdit === true && window.launcherCoreState?.canEditWorlds === true && !worldPending; }
function worldPayload(op, target = worldTarget, extra = {}) {
  return {op,activeId:target?.activeId ?? worldSnapshot.activeId,revision:target?.revision ?? worldSnapshot.revision,
    ...(target?.id ? {world:target.id,version:target.version} : {}),...extra};
}
function sendWorld(op, payload, picker = false) {
  if (worldPending) return;
  worldPending = ++worldSerial; worldPendingOp = op;
  document.getElementById("world-notice").textContent = op === "list" ? "ワールドを読み込み中…" : "処理中…";
  document.getElementById("world-dialog-notice").textContent = op === "list" ? "" : "処理中…";
  updateWorldControls();
  windowCommand(`world:${picker ? "select-image" : "command"}:${worldPending}:${JSON.stringify(payload)}`);
}
function updateWorldControls() {
  const enabled = worldCanEdit();
  document.getElementById("world-refresh").disabled = !!worldPending || window.launcherCoreState?.corePhase !== "ready";
  worldList.setAttribute("aria-busy", String(!!worldPending));
  for (const button of worldList.querySelectorAll("button")) button.disabled = !enabled;
  const current = worldTarget && worldSnapshot && worldTarget.activeId === worldSnapshot.activeId && worldTarget.revision === worldSnapshot.revision;
  worldName.disabled = worldDestination.disabled = document.getElementById("world-select-image").disabled = !enabled;
  const recover = worldTarget?.op === "recover";
  worldConfirm.disabled = !current || (recover ? !!worldPending || window.launcherCoreState?.canRecoverWorlds !== true : !enabled) ||
    (worldTarget?.op === "rename" && !validWorldName(worldName.value)) || (worldTarget?.op === "icon" && worldSnapshot?.preview?.world !== worldTarget.id) ||
    (worldTarget?.op === "move" && !worldSnapshot.accounts.some(account => account.id === Number(worldDestination.value)));
  document.getElementById("world-cancel").disabled = !!worldPending && worldPendingOp !== "list";
  document.getElementById("world-form").setAttribute("aria-busy", String(!!worldPending));
  document.getElementById("world-recover").disabled = !!worldPending || window.launcherCoreState?.canRecoverWorlds !== true;
}
function renderWorlds() {
  const state = worldSnapshot;
  const active = accountSnapshot?.accounts?.find(account => account.id === state?.activeId);
  document.getElementById("world-account").textContent = state ? `${active?.name || "Active Account"} · ${state.activeId.toString(16).padStart(8,"0")}` : "";
  document.getElementById("world-recover").hidden = !state?.recoveryRequired;
  const rows = document.createDocumentFragment();
  for (const world of state?.worlds || []) {
    const card = worldNode("article", undefined, "world-card");
    if (world.icon) { const image = document.createElement("img"); image.src = world.icon; image.alt = ""; image.width = image.height = 64; card.append(image); }
    else card.append(worldNode("div", "?", "world-icon-empty"));
    const info = worldNode("div", undefined, "world-info");
    info.append(worldNode("h2",world.name || `読み込めないワールド · ${world.id}`));
    info.append(worldNode("p",world.problem ? "ファイルを保持しています。編集はできません。" : `${new Intl.NumberFormat("ja-JP",{maximumFractionDigits:2}).format(world.size / 1048576)} MB`,"footnote"));
    card.append(info);
    if (!world.problem) {
      const actions = worldNode("div", undefined, "world-actions");
      for (const [op,label] of [["rename","Rename"],["icon","Change Icon"],["move","Move"]]) {
        const button = worldNode("button",label); button.type = "button"; button.dataset.worldOperation = op; button.dataset.world = world.id;
        button.setAttribute("aria-label",`${label} · ${world.name}`); actions.append(button);
      }
      card.append(actions);
    }
    rows.append(card);
  }
  if (!state?.worlds.length) rows.append(worldNode("p",state ? "このアカウントにはワールドがありません。" : "準備完了後にワールドを表示します。","friends-empty"));
  worldList.replaceChildren(rows); updateWorldControls();
}
function openWorldDialog(op, world, opener = document.activeElement) {
  if (op === "recover" ? !worldSnapshot?.recoveryRequired || worldPending : !worldCanEdit()) return;
  worldTarget = {...world,op,activeId:worldSnapshot.activeId,revision:worldSnapshot.revision}; worldFocus = opener;
  document.getElementById("world-dialog-title").textContent = {rename:"Rename World",icon:"Change Icon",move:"Move World",recover:"ワールドの復旧"}[op];
  document.getElementById("world-dialog-description").textContent = op === "recover" ? "バックアップと現在のファイルを照合し、中断した操作を取り消して変更前へ復旧します。状態が一致しない場合はファイルを変更せず停止します。" : op === "move" ? "選択したワールドだけを、次のアカウントへ移動します。移動先の既存ワールドは保持します。" : "ゲーム内のワールド一覧にも反映されます。";
  document.getElementById("world-dialog-target").textContent = world?.name || "";
  document.getElementById("world-name-row").hidden = op !== "rename";
  document.getElementById("world-icon-row").hidden = op !== "icon";
  document.getElementById("world-move-row").hidden = op !== "move";
  worldName.value = world?.name || "";
  worldDestination.replaceChildren(...worldSnapshot.accounts.map(account => {
    const option = worldNode("option",`${account.name} · ${account.id.toString(16).padStart(8,"0")}`); option.value = String(account.id); return option;
  }));
  const image = document.getElementById("world-icon-preview"); image.removeAttribute("src"); if (op === "icon" && world?.icon) image.src = world.icon;
  document.getElementById("world-dialog-notice").textContent = op === "move" && !worldSnapshot.accounts.length ? "ほかの登録済みアカウントがありません。" : "";
  worldConfirm.textContent = op === "move" ? "このAccountへ移動" : op === "recover" ? "変更前へ復旧" : "Save";
  worldDialog.showModal(); updateWorldControls();
  (op === "rename" ? worldName : document.getElementById("world-cancel")).focus();
}
function updateWorldView() {
  const visible = selectedPage === "world" && !document.hidden;
  if (!visible) { worldVisible = false; if (worldDialog.open && !worldPending) worldDialog.close(); return; }
  updateWorldControls();
  if (window.launcherCoreState?.phase === "unavailable") { document.getElementById("world-notice").textContent = "Coreに接続するとワールドを表示できます。"; return; }
  if (!worldVisible && !worldPending && window.launcherCoreState?.corePhase === "ready") { worldVisible = true; sendWorld("list",{op:"list"}); }
}
window.updateWorldView = updateWorldView;
worldList.addEventListener("click", event => {
  const button = event.target.closest("[data-world-operation]"); if (!button || button.disabled) return;
  const world = worldSnapshot?.worlds.find(world => world.id === button.dataset.world); if (world) openWorldDialog(button.dataset.worldOperation,world,button);
});
document.getElementById("world-refresh").addEventListener("click", () => sendWorld("list",{op:"list"}));
document.getElementById("world-recover").addEventListener("click", () => openWorldDialog("recover"));
document.getElementById("world-select-image").addEventListener("click", () => { if (worldCanEdit()) sendWorld("preview",worldPayload("preview"),true); });
document.getElementById("world-cancel").addEventListener("click", () => { if (!worldPending) worldDialog.close(); });
worldDialog.addEventListener("cancel", event => { if (worldPending) event.preventDefault(); });
worldDialog.addEventListener("close", () => {
  const target = worldTarget; worldTarget = null;
  if (selectedPage === "world") (worldFocus?.isConnected && !worldFocus.disabled ? worldFocus : document.getElementById("world-refresh")).focus({preventScroll:true});
  if (target?.op === "icon" && worldSnapshot?.preview && !worldPending) sendWorld("clear-preview",worldPayload("clear-preview",target));
});
worldName.addEventListener("input", () => { document.getElementById("world-dialog-notice").textContent = validWorldName(worldName.value) ? "" : worldNotices["invalid-name"]; updateWorldControls(); });
worldDestination.addEventListener("change", updateWorldControls);
document.getElementById("world-form").addEventListener("submit", event => {
  event.preventDefault(); if (worldConfirm.disabled || !worldTarget) return;
  const op = worldTarget.op;
  const extra = op === "rename" ? {name:worldName.value} : op === "icon" ? {token:worldSnapshot.preview.token} : op === "move" ? {destination:Number(worldDestination.value)} : {};
  sendWorld(op,worldPayload(op,worldTarget,extra));
});
worldDialog.addEventListener("keydown", event => {
  if (event.key !== "Tab") return;
  const controls = [...worldDialog.querySelectorAll("input,select,button")].filter(node => !node.disabled && node.getClientRects().length);
  if (event.shiftKey && document.activeElement === controls[0]) { event.preventDefault(); controls.at(-1)?.focus(); }
  else if (!event.shiftKey && document.activeElement === controls.at(-1)) { event.preventDefault(); controls[0]?.focus(); }
});
window.addEventListener("launcher-world-result", event => {
  if (event.detail?.requestId !== worldPending) return;
  const state = event.detail.world, op = worldPendingOp; worldPending = 0; worldPendingOp = "";
  if (!state || !Number.isSafeInteger(state.revision) || !Number.isInteger(state.activeId) || !Array.isArray(state.worlds) || !Array.isArray(state.accounts) || state.worlds.length > 1000 ||
      state.worlds.some(world => !/^[0-9]{12}$/.test(world.id) || typeof world.name !== "string" || typeof world.version !== "string" || (world.icon && !/^data:image\/png;base64,[A-Za-z0-9+/=]+$/.test(world.icon)))) {
    document.getElementById("world-notice").textContent = worldMessage(state?.notice); updateWorldControls(); return;
  }
  worldSnapshot = state;
  if (worldTarget && (worldTarget.activeId !== state.activeId || worldTarget.revision !== state.revision)) worldDialog.close();
  document.getElementById("world-notice").textContent = state.recoveryRequired ? worldNotices["recovery-required"] : worldMessage(state.notice);
  if (worldDialog.open) {
    document.getElementById("world-dialog-notice").textContent = worldMessage(state.notice);
    if (op === "preview" && state.preview) document.getElementById("world-icon-preview").src = state.preview.icon;
    if (["saved","unchanged","recovered"].includes(state.notice)) worldDialog.close();
  }
  renderWorlds();
});
window.addEventListener("launcher-account-result", event => {
  const state = event.detail?.account;
  if (state && worldSnapshot && (state.activeId !== worldSnapshot.activeId || worldTarget)) {
    if (worldDialog.open && !worldPending) worldDialog.close(); worldSnapshot = null; worldVisible = false; renderWorlds(); updateWorldView();
  }
});
window.addEventListener("launcher-core-state", updateWorldView);
document.addEventListener("visibilitychange", updateWorldView);
