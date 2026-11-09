"use strict";

const worldList = document.getElementById("world-list"), worldDialog = document.getElementById("world-dialog");
const worldName = document.getElementById("world-name"), worldConfirm = document.getElementById("world-confirm"), worldDestination = document.getElementById("world-destination");
let worldSnapshot = null, worldSerial = Date.now(), worldPending = 0, worldPendingOp = "", worldTarget = null, worldFocus = null, worldVisible = false, worldView = "worlds";
const worldImportDialog = document.getElementById("world-import-dialog"), worldImportConfirm = document.getElementById("world-import-confirm");
let worldImportTarget = null;
const worldNotices = {
  ready:"", saved:"保存しました。", unchanged:"変更はありません。", cancelled:"操作を取り消しました。", "preview-ready":"プレビューを確認して Save を押してください。",
  deleted:"Trashへ移動しました。", restored:"元のAccountへ復元しました。", imported:"ワールドをインポートしました。", "import-preview-ready":"候補とインポート先を確認して Import を押してください。",
  "not-ready":"ゲームと終了処理が停止し、準備完了になってから操作してください。", "stale-request":"アカウントまたは一覧が変わりました。Refresh して操作し直してください。",
  "stale-world":"ワールドが更新されました。Refresh して操作し直してください。", "invalid-name":"名前は1〜32 UTF-16単位で入力してください。制御文字や空白だけの名前は使えません。",
  "save-id-conflict":"同じSave IDが移動先に存在します。上書きせず中止しました。", "invalid-image":"画像を読み込めません。16 MB以下のPNG / JPEGを選択してください。",
  "unsupported-metadata":"この保存形式は安全に編集できません。", "invalid-world":"このワールドを安全に読み込めません。", "invalid-account":"アカウントを確認できません。",
  "unsafe-path":"安全な保存先を確認できません。", "stale-preview":"画像プレビューが古くなりました。画像を選び直してください。",
  "recovery-required":"中断した操作があります。復旧が完了するまでゲーム起動と編集を停止しています。",
  recovered:"変更前のワールドを復旧しました。", "rolled-back":"操作に失敗したため、変更前に復旧しました。", "read-failed":"ファイルを読み込めません。ほかのアプリで使用中の場合は閉じてください。",
  "write-failed":"書き込みに失敗しました。保存先の空き容量や使用状況を確認してください。", "verification-failed":"書き込み後の整合性を確認できませんでした。",
  "invalid-file":"ファイルの形式やサイズを確認できません。", "world-limit":"ワールド数が上限を超えています。", "invalid-request":"操作を検証できません。Refresh してください。",
  "insufficient-space":"保存先の空き容量が不足しています。", "invalid-trash":"保管データの整合性を確認できません。ファイルは保持しています。", "stale-trash":"Trashの状態が変わりました。Refresh してください。",
  "trash-unavailable":"この保管データは復元済み、または操作未完了です。", "trash-count-limit":"Trashの一覧上限を超えています。データは削除しません。",
  "stale-import-preview":"プレビューが古くなりました。インポート元を選び直してください。", "stale-import-source":"元のファイルが変更されました。インポート元を選び直してください。",
  "invalid-body":"本体の圧縮データやファイル索引が不正です。", "unsupported-body-format":"この本体形式は未検証です。形式変換は行いません。",
  "body-size-limit":"本体の展開サイズが上限を超えています。", "unsafe-source-path":"安全なインポート元を確認できません。リンクは使用できません。",
  "invalid-save-id":"12桁のSave IDを持つ拡張子なし本体または.extを選択してください。", "missing-pair":"同じ場所にある対応する拡張子なし本体または.extが見つかりません。",
  "unsupported-title":"別リージョンのタイトルフォルダには対応していません。", "unsupported-import-format":"拡張子なしの12桁の本体と.extだけを読み込めます。Folder・ZIP・.wiiには対応していません。",
  "invalid-import-source":"インポート元を検証できません。", "trash-empty":"Trashは空です。", "purged":"確認した保管データを完全削除しました。",
  "purge-preview-ready":"対象と容量を確認してください。完全削除は取り消せません。", "purge-blocked":"安全に削除できない項目があるため、削除を開始していません。",
  "purge-incomplete":"取引未完了の保管項目です。", "purge-owner-mismatch":"元の取引との対応を確認できません。", "purge-unknown-file":"所有を確認できないファイルがあります。",
  "unsafe-purge-file":"リンク・読み取り専用など、安全に削除できないファイルがあります。", "purge-file-busy":"ファイルが使用中のため削除できません。",
  "purge-changed":"確認後に対象ファイルやTrashの構成が変わりました。Refreshして操作し直してください。", "purge-write-failed":"完全削除が中断しました。残りの処理には明示的な復旧操作が必要です。",
  "purge-backup-referenced":"他の取引の参照、または参照不明のバックアップがあるため、関連バックアップは保持します。",
  "purge-backup-protected":"関連バックアップを安全に整理できないため保持します。", "purge-backup-limit":"バックアップ数が上限を超えています。",
  "purge-protected":"保管データを安全に削除できません。ファイルは保持しています。", "invalid-purge":"完全削除の取引情報を検証できません。",
  "stale-purge-preview":"削除確認が古くなりました。対象を確認し直してください。",
  "purge-plan-limit":"削除計画の容量が上限を超えています。個別Deleteを使用してください。"
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
function sendWorld(op, payload, picker = false, importPicker = false) {
  if (worldPending) return;
  worldPending = ++worldSerial; worldPendingOp = op;
  document.getElementById("world-notice").textContent = op === "list" ? "ワールドを読み込み中…" : "処理中…";
  document.getElementById("world-dialog-notice").textContent = op === "list" ? "" : "処理中…";
  if (worldImportDialog.open) document.getElementById("world-import-notice").textContent = "検証中…";
  updateWorldControls();
  windowCommand(`world:${importPicker ? "select-import" : picker ? "select-image" : "command"}:${worldPending}:${JSON.stringify(payload)}`);
}
function updateWorldControls() {
  const enabled = worldCanEdit();
  document.getElementById("world-refresh").disabled = !!worldPending || window.launcherCoreState?.corePhase !== "ready";
  document.getElementById("world-import").disabled = !enabled;
  document.getElementById("world-trash").disabled = !!worldPending || window.launcherCoreState?.corePhase !== "ready";
  document.getElementById("world-back").disabled = !!worldPending;
  worldList.setAttribute("aria-busy", String(!!worldPending));
  for (const button of worldList.querySelectorAll("button")) button.disabled = !enabled || button.dataset.purgeBlocked === "true";
  document.getElementById("world-empty-trash").disabled = !enabled || !worldSnapshot?.trash.length;
  const current = worldTarget && worldSnapshot && worldTarget.activeId === worldSnapshot.activeId && worldTarget.revision === worldSnapshot.revision;
  worldName.disabled = worldDestination.disabled = document.getElementById("world-select-image").disabled = !enabled;
  const recover = worldTarget?.op === "recover";
  worldConfirm.disabled = !current || (recover ? !!worldPending || window.launcherCoreState?.canRecoverWorlds !== true : !enabled) ||
    (worldTarget?.op === "rename" && !validWorldName(worldName.value)) || (worldTarget?.op === "icon" && worldSnapshot?.preview?.world !== worldTarget.id) ||
    (worldTarget?.op === "move" && !worldSnapshot.accounts.some(account => account.id === Number(worldDestination.value))) ||
    (worldTarget?.op === "restore" && (!worldTarget.accountAvailable || worldTarget.problem || worldTarget.status !== "deleted")) ||
    (["purge","empty-trash"].includes(worldTarget?.op) && (!worldSnapshot?.purgePreview || worldSnapshot.purgeIssues.length > 0));
  document.getElementById("world-cancel").disabled = !!worldPending && worldPendingOp !== "list";
  document.getElementById("world-form").setAttribute("aria-busy", String(!!worldPending));
  document.getElementById("world-recover").disabled = !!worldPending || window.launcherCoreState?.canRecoverWorlds !== true;
  const importCurrent = worldImportTarget && worldSnapshot && worldImportTarget.activeId === worldSnapshot.activeId && worldImportTarget.revision === worldSnapshot.revision;
  worldImportConfirm.disabled = !enabled || !importCurrent || !worldSnapshot?.importPreview?.candidate;
  document.getElementById("world-import-select").disabled = !enabled || !importCurrent;
  document.getElementById("world-import-cancel").disabled = !!worldPending;
  document.getElementById("world-import-form").setAttribute("aria-busy", String(!!worldPending));
}
function renderWorlds() {
  const state = worldSnapshot;
  const active = accountSnapshot?.accounts?.find(account => account.id === state?.activeId);
  document.getElementById("world-account").textContent = state ? `${active?.name || "Active Account"} · ${state.activeId.toString(16).padStart(8,"0")}` : "";
  document.getElementById("world-recover").hidden = !state?.recoveryRequired;
  document.getElementById("world-title").textContent = worldView === "trash" ? "Recently Deleted" : "World";
  document.getElementById("world-import").hidden = document.getElementById("world-trash").hidden = worldView === "trash";
  document.getElementById("world-back").hidden = document.getElementById("world-empty-trash").hidden = worldView !== "trash";
  document.getElementById("world-trash-capacity").hidden = worldView !== "trash";
  document.getElementById("world-trash-capacity").textContent = `Stored: ${state?.trash.length || 0} Worlds · 保管容量（復元済み含む・確認できた項目のみ）: ${new Intl.NumberFormat("ja-JP",{maximumFractionDigits:2}).format((state?.trashBytes || 0) / 1048576)} MiB`;
  const rows = document.createDocumentFragment();
  for (const world of (worldView === "trash" ? state?.trash : state?.worlds) || []) {
    const card = worldNode("article", undefined, "world-card");
    if (world.icon) { const image = document.createElement("img"); image.src = world.icon; image.alt = ""; image.width = image.height = 64; card.append(image); }
    else card.append(worldNode("div", "?", "world-icon-empty"));
    const info = worldNode("div", undefined, "world-info");
    info.append(worldNode("h2",world.name || `読み込めないワールド · ${world.id}`));
    info.append(worldNode("p",world.problem ? "ファイルを保持しています。編集はできません。" : `${new Intl.NumberFormat("ja-JP",{maximumFractionDigits:2}).format(world.size / 1048576)} MB`,"footnote"));
    if (worldView === "trash" && !world.problem) {
      info.append(worldNode("p",`Original Account · ${world.account.toString(16).padStart(8,"0")} · Save ID ${world.id}`,"footnote"));
      if (!world.accountAvailable) info.append(worldNode("p","元のAccountが登録されていないため復元できません。","footnote"));
      info.append(worldNode("p",`Deleted · ${new Date(world.deletedAt).toLocaleString("ja-JP")}${world.status === "restored" ? " · 復元済み・保管中" : world.status === "cancelled" ? " · 操作取消・保管中" : world.status === "incomplete" ? " · 取引未完了" : ""}`,"footnote"));
    }
    card.append(info);
    if (worldView === "trash" && (world.problem || world.purgeProblem)) info.append(worldNode("p",`完全削除不可 · ${worldMessage(world.purgeProblem || "invalid-trash")}`,"footnote"));
    if (!world.problem || worldView === "trash") {
      const actions = worldNode("div", undefined, "world-actions");
      const operations = worldView === "trash" ? [...(!world.problem && world.status === "deleted" && world.accountAvailable ? [["restore","Restore"]] : []),["purge","Delete"]] : [["rename","Rename"],["icon","Change Icon"],["move","Move"],["delete","Delete"]];
      for (const [op,label] of operations) {
        const button = worldNode("button",label); button.type = "button"; button.dataset.worldOperation = op; button.dataset.world = world.id;
        button.setAttribute("aria-label",`${op === "purge" ? "Delete Permanently" : label} · ${world.name}`); actions.append(button);
        if (["delete","purge"].includes(op)) button.classList.add("world-danger");
        if (op === "purge") button.dataset.purgeBlocked = String(!!world.problem || !!world.purgeProblem);
        if (worldView === "trash") button.dataset.trash = world.key;
      }
      card.append(actions);
    }
    rows.append(card);
  }
  if (!(worldView === "trash" ? state?.trash?.length : state?.worlds.length)) rows.append(worldNode("p",state ? worldView === "trash" ? "削除済みワールドはありません。" : "このアカウントにはワールドがありません。" : "準備完了後にワールドを表示します。","friends-empty"));
  worldList.replaceChildren(rows);
  const progress = document.getElementById("world-purge-progress"), counts = state?.purgeProgress;
  progress.hidden = state?.recoveryMode !== "purge" || !state?.recoveryRequired;
  progress.textContent = counts ? `完全削除が中断しています。完了 ${counts.completed} / ${counts.total}件、未完了 ${counts.remaining}件（部分削除 ${counts.partial}件）、保護/停止理由 ${counts.protected}件。復旧は残りの削除を続行します。` : "";
  if (!progress.hidden && state.purgeIssues?.length) progress.textContent += " " + state.purgeIssues.map(issue => `${issue.name || issue.key || "対象データ"}: ${worldMessage(issue.reason)}`).join(" / ");
  updateWorldControls();
}
function openWorldDialog(op, world, opener = document.activeElement) {
  if (op === "recover" ? !worldSnapshot?.recoveryRequired || worldPending : !worldCanEdit()) return;
  worldTarget = {...world,op,activeId:worldSnapshot.activeId,revision:worldSnapshot.revision}; worldFocus = opener;
  document.getElementById("world-dialog-title").textContent = {rename:"Rename World",icon:"Change Icon",move:"Move World",recover:"ワールドの復旧",delete:"Delete World",restore:"Restore World",purge:"Delete Permanently", "empty-trash":"Empty Trash"}[op];
  document.getElementById("world-dialog-description").textContent = op === "recover" ? "バックアップと現在のファイルを照合し、中断した操作を取り消して変更前へ復旧します。状態が一致しない場合はファイルを変更せず停止します。" : op === "move" ? "選択したワールドだけを、次のアカウントへ移動します。移動先の既存ワールドは保持します。" : op === "delete" ? "このワールドはゲーム内一覧から消えます。本体と.extはTrashに保管され、後から元のAccountへ復元できます。" : op === "restore" ? "削除前のAccountへ復元します。既存ワールドの上書きは行いません。" : "ゲーム内のワールド一覧にも反映されます。";
  if (["purge","empty-trash"].includes(op)) document.getElementById("world-dialog-description").textContent = (op === "purge" ? "このワールドを完全に削除します。この操作は取り消せません。" : "ごみ箱内のワールドを完全に削除します。この操作は取り消せません。") + "Trashの保管分と直接紐づく取引コピーが対象です。復元済みワールドや独立した過去のバックアップは保持します。";
  if (op === "recover" && worldSnapshot.recoveryMode === "purge") document.getElementById("world-dialog-description").textContent = "中断した完全削除を続行します。削除済みのデータを復元せず、永続化された対象と残りのファイルを照合して処理します。";
  document.getElementById("world-purge-issues").hidden = !["purge","empty-trash"].includes(op); document.getElementById("world-purge-issues").replaceChildren();
  document.getElementById("world-dialog-target").textContent = world?.name || "";
  document.getElementById("world-name-row").hidden = op !== "rename";
  document.getElementById("world-icon-row").hidden = op !== "icon";
  document.getElementById("world-move-row").hidden = op !== "move";
  document.getElementById("world-confirm-details").hidden = !["delete","restore","purge"].includes(op);
  const confirmIcon = document.getElementById("world-confirm-icon"); confirmIcon.removeAttribute("src"); if (["delete","restore","purge"].includes(op) && world?.icon) confirmIcon.src = world.icon;
  const owner = ["restore","purge"].includes(op) ? world.account : worldSnapshot.activeId;
  const ownerName = accountSnapshot?.accounts?.find(account => account.id === owner)?.name || "Account";
  document.getElementById("world-confirm-location").textContent = world ? `Save ID · ${world.id}\n${ownerName} · ${owner.toString(16).padStart(8,"0")}${op === "purge" ? `\n${(world.size / 1048576).toFixed(2)} MiB` : ""}` : "";
  worldName.value = world?.name || "";
  worldDestination.replaceChildren(...worldSnapshot.accounts.map(account => {
    const option = worldNode("option",`${account.name} · ${account.id.toString(16).padStart(8,"0")}`); option.value = String(account.id); return option;
  }));
  const image = document.getElementById("world-icon-preview"); image.removeAttribute("src"); if (op === "icon" && world?.icon) image.src = world.icon;
  document.getElementById("world-dialog-notice").textContent = op === "move" && !worldSnapshot.accounts.length ? "ほかの登録済みアカウントがありません。" : "";
  worldConfirm.textContent = op === "move" ? "このAccountへ移動" : op === "recover" ? "変更前へ復旧" : op === "delete" ? "Delete World" : op === "restore" ? "Restore World" : op === "purge" ? "Delete Permanently" : op === "empty-trash" ? "Empty Trash" : "Save";
  if (op === "recover" && worldSnapshot.recoveryMode === "purge") { worldConfirm.textContent = "残りの削除を続行"; document.getElementById("world-dialog-title").textContent = "完全削除の続行"; }
  worldConfirm.classList.toggle("world-danger",["delete","purge","empty-trash"].includes(op));
  worldDialog.showModal(); updateWorldControls();
  (op === "rename" ? worldName : document.getElementById("world-cancel")).focus();
  if (["purge","empty-trash"].includes(op)) sendWorld("prepare-purge",{op:"prepare-purge",activeId:worldTarget.activeId,revision:worldTarget.revision,...(op === "purge" ? {trash:world.key,version:world.version} : {})});
}
function renderWorldImport() {
  if (!worldImportDialog.open) return;
  const candidate = worldSnapshot?.importPreview?.candidate, row = worldNode("div",undefined,"world-import-candidate");
  if (candidate) {
    const image = document.createElement("img"); image.src = candidate.icon; image.alt = ""; row.append(image);
    const info = worldNode("div"); info.append(worldNode("strong",candidate.name));
    info.append(worldNode("p",`Save ID · ${candidate.id} · Source Account · ${candidate.account || "不明"}`));
    info.append(worldNode("p",`${(candidate.size / 1048576).toFixed(2)} MiB · コンテナ・メタデータ検証済み`)); row.append(info);
  }
  document.getElementById("world-import-preview").replaceChildren(...(candidate ? [row] : [])); updateWorldControls();
}
function selectWorldImport() {
  if (worldCanEdit() && worldImportTarget) sendWorld("preview-import",{op:"preview-import",activeId:worldImportTarget.activeId,revision:worldImportTarget.revision},false,true);
}
document.getElementById("world-import").addEventListener("click", () => {
  if (!worldCanEdit()) return;
  worldImportTarget = {activeId:worldSnapshot.activeId,revision:worldSnapshot.revision};
  const name = accountSnapshot?.accounts?.find(account => account.id === worldSnapshot.activeId)?.name || "Active Account";
  document.getElementById("world-import-destination").textContent = `Import to · ${name} · ${worldSnapshot.activeId.toString(16).padStart(8,"0")}`;
  document.getElementById("world-import-notice").textContent = "拡張子なしの本体または.extを選択してください。";
  worldImportDialog.showModal(); renderWorldImport(); selectWorldImport();
});
document.getElementById("world-import-select").addEventListener("click",selectWorldImport);
document.getElementById("world-import-cancel").addEventListener("click", () => { if (!worldPending) worldImportDialog.close(); });
worldImportDialog.addEventListener("cancel", event => { if (worldPending) event.preventDefault(); });
worldImportDialog.addEventListener("close", () => {
  worldImportTarget = null;
  if (selectedPage === "world") document.getElementById("world-import").focus({preventScroll:true});
  if (worldSnapshot?.importPreview && !worldPending) sendWorld("clear-import",{op:"clear-import",activeId:worldSnapshot.activeId,revision:worldSnapshot.revision});
});
worldImportDialog.addEventListener("keydown", event => {
  if (event.key !== "Tab") return;
  const controls = [...worldImportDialog.querySelectorAll("input,button")].filter(node => !node.disabled && node.getClientRects().length);
  if (event.shiftKey && document.activeElement === controls[0]) { event.preventDefault(); controls.at(-1)?.focus(); }
  else if (!event.shiftKey && document.activeElement === controls.at(-1)) { event.preventDefault(); controls[0]?.focus(); }
});
document.getElementById("world-import-form").addEventListener("submit", event => {
  event.preventDefault(); if (worldImportConfirm.disabled || !worldImportTarget || !worldSnapshot?.importPreview) return;
  sendWorld("import",{op:"import",activeId:worldImportTarget.activeId,revision:worldImportTarget.revision,token:worldSnapshot.importPreview.token});
});
function updateWorldView() {
  const visible = selectedPage === "world" && !document.hidden;
  if (!visible) { worldVisible = false; if (!worldPending) { if (worldDialog.open) worldDialog.close(); if (worldImportDialog.open) worldImportDialog.close(); } return; }
  updateWorldControls();
  if (window.launcherCoreState?.phase === "unavailable") { document.getElementById("world-notice").textContent = "Coreに接続するとワールドを表示できます。"; return; }
  if (!worldVisible && !worldPending && window.launcherCoreState?.corePhase === "ready") { worldVisible = true; sendWorld(worldView === "trash" ? "trash-list" : "list",{op:worldView === "trash" ? "trash-list" : "list"}); }
}
window.updateWorldView = updateWorldView;
worldList.addEventListener("click", event => {
  const button = event.target.closest("[data-world-operation]"); if (!button || button.disabled) return;
  const world = button.dataset.trash ? worldSnapshot?.trash.find(world => world.key === button.dataset.trash) : worldSnapshot?.worlds.find(world => world.id === button.dataset.world); if (world) openWorldDialog(button.dataset.worldOperation,world,button);
});
document.getElementById("world-refresh").addEventListener("click", () => { const op = worldView === "trash" ? "trash-list" : "list"; sendWorld(op,{op}); });
document.getElementById("world-trash").addEventListener("click", () => { worldView = "trash"; renderWorlds(); sendWorld("trash-list",{op:"trash-list"}); document.getElementById("world-back").focus(); });
document.getElementById("world-back").addEventListener("click", () => { worldView = "worlds"; renderWorlds(); sendWorld("list",{op:"list"}); document.getElementById("world-trash").focus(); });
document.getElementById("world-empty-trash").addEventListener("click", () => openWorldDialog("empty-trash"));
document.getElementById("world-recover").addEventListener("click", () => openWorldDialog("recover"));
document.getElementById("world-select-image").addEventListener("click", () => { if (worldCanEdit()) sendWorld("preview",worldPayload("preview"),true); });
document.getElementById("world-cancel").addEventListener("click", () => { if (!worldPending) worldDialog.close(); });
worldDialog.addEventListener("cancel", event => { if (worldPending) event.preventDefault(); });
worldDialog.addEventListener("close", () => {
  const target = worldTarget; worldTarget = null;
  if (selectedPage === "world") (worldFocus?.isConnected && !worldFocus.disabled ? worldFocus : document.getElementById("world-refresh")).focus({preventScroll:true});
  if (["purge","empty-trash"].includes(target?.op) && !worldPending && worldSnapshot?.purgePreview) sendWorld("clear-purge",{op:"clear-purge",activeId:worldSnapshot.activeId,revision:worldSnapshot.revision});
  if (target?.op === "icon" && worldSnapshot?.preview && !worldPending) sendWorld("clear-preview",worldPayload("clear-preview",target));
});
worldName.addEventListener("input", () => { document.getElementById("world-dialog-notice").textContent = validWorldName(worldName.value) ? "" : worldNotices["invalid-name"]; updateWorldControls(); });
worldDestination.addEventListener("change", updateWorldControls);
document.getElementById("world-form").addEventListener("submit", event => {
  event.preventDefault(); if (worldConfirm.disabled || !worldTarget) return;
  const op = worldTarget.op;
  const extra = op === "rename" ? {name:worldName.value} : op === "icon" ? {token:worldSnapshot.preview.token} : op === "move" ? {destination:Number(worldDestination.value)} : {};
  if (["purge","empty-trash"].includes(op)) { sendWorld("purge",{op:"purge",activeId:worldTarget.activeId,revision:worldTarget.revision,token:worldSnapshot.purgePreview.token}); return; }
  sendWorld(op,op === "restore" ? {op,activeId:worldTarget.activeId,revision:worldTarget.revision,trash:worldTarget.key,version:worldTarget.version} : worldPayload(op,worldTarget,extra));
});
worldDialog.addEventListener("keydown", event => {
  if (event.key === "Enter" && ["purge","empty-trash"].includes(worldTarget?.op)) { event.preventDefault(); return; }
  if (event.key !== "Tab") return;
  const controls = [...worldDialog.querySelectorAll("input,select,button")].filter(node => !node.disabled && node.getClientRects().length);
  if (event.shiftKey && document.activeElement === controls[0]) { event.preventDefault(); controls.at(-1)?.focus(); }
  else if (!event.shiftKey && document.activeElement === controls.at(-1)) { event.preventDefault(); controls[0]?.focus(); }
});
window.addEventListener("launcher-world-result", event => {
  if (event.detail?.requestId !== worldPending) return;
  const state = event.detail.world, op = worldPendingOp; worldPending = 0; worldPendingOp = "";
  if (!state || !Number.isSafeInteger(state.revision) || !Number.isInteger(state.activeId) || !Array.isArray(state.worlds) || !Array.isArray(state.accounts) || state.worlds.length > 1000 ||
      state.worlds.some(world => !/^[0-9]{12}$/.test(world.id) || typeof world.name !== "string" || typeof world.version !== "string" || (world.icon && !/^data:image\/png;base64,[A-Za-z0-9+/=]+$/.test(world.icon))) ||
      !Array.isArray(state.trash) || state.trash.length > 4096 || state.trash.some(entry => typeof entry.key !== "string" || typeof entry.name !== "string" || (entry.icon && !/^data:image\/png;base64,[A-Za-z0-9+/=]+$/.test(entry.icon))) ||
      (state.importPreview && (typeof state.importPreview.token !== "string" || !state.importPreview.candidate || !/^[0-9]{12}$/.test(state.importPreview.candidate.id) || typeof state.importPreview.candidate.name !== "string" || !/^data:image\/png;base64,[A-Za-z0-9+/=]+$/.test(state.importPreview.candidate.icon)))) {
    document.getElementById("world-notice").textContent = worldMessage(state?.notice); updateWorldControls(); return;
  }
  worldSnapshot = state;
  if (worldTarget && (worldTarget.activeId !== state.activeId || worldTarget.revision !== state.revision)) worldDialog.close();
  if (worldImportTarget && (worldImportTarget.activeId !== state.activeId || worldImportTarget.revision !== state.revision)) worldImportDialog.close();
  document.getElementById("world-notice").textContent = state.recoveryRequired ? worldNotices["recovery-required"] : worldMessage(state.notice);
  if (worldDialog.open) {
    document.getElementById("world-dialog-notice").textContent = worldMessage(state.notice);
    if (["purge","empty-trash"].includes(worldTarget?.op)) {
      if (state.purgePreview && worldTarget.op === "empty-trash") document.getElementById("world-dialog-target").textContent = `ごみ箱内の${state.purgePreview.count}個のワールド · ${(state.purgePreview.size / 1048576).toFixed(2)} MiB`;
      const notes = [...(state.purgeIssues || []),...(state.purgePreview?.retainedBackups || [])];
      document.getElementById("world-purge-issues").replaceChildren(...notes.map(issue => worldNode("p",`${issue.name || issue.key || "対象データ"} · ${worldMessage(issue.reason)}`)));
      if (op === "prepare-purge") { updateWorldControls(); document.getElementById("world-cancel").focus(); }
    }
    if (op === "preview" && state.preview) document.getElementById("world-icon-preview").src = state.preview.icon;
    if (["saved","unchanged","recovered","deleted","restored","purged"].includes(state.notice)) worldDialog.close();
  }
  if (worldImportDialog.open) { document.getElementById("world-import-notice").textContent = worldMessage(state.notice); renderWorldImport(); if (state.notice === "imported") worldImportDialog.close(); }
  renderWorlds();
});
window.addEventListener("launcher-account-result", event => {
  const state = event.detail?.account;
  if (state && worldSnapshot && (state.activeId !== worldSnapshot.activeId || worldTarget)) {
    if (!worldPending) { if (worldDialog.open) worldDialog.close(); if (worldImportDialog.open) worldImportDialog.close(); } worldSnapshot = null; worldVisible = false; renderWorlds(); updateWorldView();
  }
});
window.addEventListener("launcher-core-state", updateWorldView);
document.addEventListener("visibilitychange", updateWorldView);
