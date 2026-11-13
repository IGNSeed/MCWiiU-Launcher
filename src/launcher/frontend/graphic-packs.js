"use strict";

let graphicPacksSnapshot = null, graphicPacksPending = 0, graphicPacksSerial = 0;
let graphicPacksVisible = false, graphicPacksCooldown = 0, graphicPacksContext = "";
const graphicPacksList = document.getElementById("graphic-packs-list");
const graphicPacksNotice = document.getElementById("graphic-packs-notice");
const graphicPacksFolder = document.getElementById("graphic-packs-open-folder");
function graphicPacksReady() { return window.launcherCoreState?.corePhase === "ready"; }
function graphicPacksEditable() {
  return !graphicPacksPending && window.launcherCoreState?.canEditGraphicPacks === true && graphicPacksSnapshot?.canEdit === true;
}
function graphicPacksMessage(value) {
  const messages = {
    saved:"保存しました。次回ゲーム起動時に反映されます。",
    "game-running-or-busy":"ゲーム停止中に変更できます。",
    "stale-revision":"設定が変更されています。一覧を開き直してください。",
    "stale-request":"要求が古いため変更しませんでした。",
    "folder-opened":"フォルダを開きました。追加したパックは次回Launcher起動時に認識します。",
    "folder-open-busy":"フォルダを開いています。",
    "invalid-pack-skipped":"読み取れないパックがあります。rules.txtを確認してLauncherを再起動してください。",
    "settings-corrupt":"設定ファイルを読み取れません。既存の設定は上書きしていません。",
    "pack-changed-restart-required":"パックが変更されています。Launcherを再起動してください。",
    "save-failed":"設定を保存できませんでした。変更前の選択を保持しています。",
    "invalid-preset":"選択できないPresetです。",
    "invalid-request":"要求を受け付けられませんでした。",
    "not-ready":"Graphic Packの管理を利用できません。"
  };
  return messages[value] || (value ? "処理できませんでした。設定ファイルとパックの状態を確認してください。" : "");
}
function updateGraphicPacksControls() {
  graphicPacksFolder.disabled = !graphicPacksReady() || !!graphicPacksPending || performance.now() < graphicPacksCooldown;
  for (const input of graphicPacksList.querySelectorAll("input, select")) input.disabled = !graphicPacksEditable();
  graphicPacksList.setAttribute("aria-busy", String(!!graphicPacksPending));
}
function renderGraphicPacks() {
  const focus = document.activeElement;
  const focusId = focus?.dataset.graphicPackId, focusCategory = focus?.dataset.graphicPackCategory;
  graphicPacksList.replaceChildren();
  const packs = graphicPacksSnapshot.packs;
  if (!packs.length) {
    const empty = document.createElement("p"); empty.className = "footnote";
    empty.textContent = "Minecraft向けのGraphic Packはインストールされていません。";
    graphicPacksList.append(empty);
  }
  for (const pack of packs) {
    const card = document.createElement("article"); card.className = "graphic-pack";
    const row = document.createElement("label"); row.className = "graphic-pack-title";
    const toggle = document.createElement("input"); toggle.type = "checkbox"; toggle.checked = pack.enabled;
    toggle.dataset.graphicPackId = pack.id;
    const name = document.createElement("span"); name.textContent = pack.name;
    const status = document.createElement("span"); status.className = "footnote"; status.textContent = pack.enabled ? "Enabled" : "Disabled";
    row.append(toggle, name, status); card.append(row);
    toggle.addEventListener("change", () => {
      const enabled = toggle.checked; toggle.checked = pack.enabled;
      if (graphicPacksEditable()) sendGraphicPacks("enable", {id:pack.id, enabled, revision:graphicPacksSnapshot.revision});
    });
    if (pack.description) { const description = document.createElement("p"); description.className = "graphic-pack-description footnote"; description.textContent = pack.description; card.append(description); }
    for (const category of pack.categories) {
      if (!category.options.length) continue;
      const label = document.createElement("label"); label.className = "graphic-pack-preset input-row";
      const title = document.createElement("span"); title.textContent = category.name || "Preset";
      const select = document.createElement("select"); select.dataset.graphicPackId = pack.id; select.dataset.graphicPackCategory = category.name;
      for (const option of category.options) { const node = document.createElement("option"); node.value = option; node.textContent = option; select.append(node); }
      select.value = category.selected;
      select.addEventListener("change", () => {
        const name = select.value; select.value = category.selected;
        if (graphicPacksEditable()) sendGraphicPacks("preset", {id:pack.id, category:category.name, name, revision:graphicPacksSnapshot.revision});
      });
      label.append(title, select); card.append(label);
    }
    graphicPacksList.append(card);
  }
  updateGraphicPacksControls();
  if (focusId) [...graphicPacksList.querySelectorAll("input, select")].find(node => node.dataset.graphicPackId === focusId && node.dataset.graphicPackCategory === focusCategory)?.focus({preventScroll:true});
}
function sendGraphicPacks(op, extra = {}) {
  if (!graphicPacksReady() || graphicPacksPending) return;
  graphicPacksPending = ++graphicPacksSerial;
  graphicPacksNotice.textContent = op === "list" ? "Graphic Packを読み込み中…" : "処理中…";
  updateGraphicPacksControls();
  windowCommand(`graphic-packs:${op === "open-folder" ? "open-folder" : "command"}:${graphicPacksPending}:${JSON.stringify(op === "open-folder" ? {} : {op,...extra})}`);
}
function updateGraphicPacksView() {
  const visible = selectedPage === "graphic-packs" && !document.hidden;
  const context = `${window.launcherCoreState?.phase}|${window.launcherCoreState?.canEditGraphicPacks}`;
  if (visible && (!graphicPacksVisible || context !== graphicPacksContext) && graphicPacksReady() && !graphicPacksPending) {
    graphicPacksVisible = true; graphicPacksContext = context; sendGraphicPacks("list");
  } else if (!visible) graphicPacksVisible = false;
  if (visible && !graphicPacksReady()) graphicPacksNotice.textContent = window.launcherCoreState?.phase === "unavailable" ? "CoreがないためGraphic Packの管理は利用できません。" : "準備完了後にGraphic Packを表示します。";
  updateGraphicPacksControls();
}
graphicPacksFolder.addEventListener("click", () => {
  if (graphicPacksFolder.disabled) return;
  graphicPacksCooldown = performance.now() + 1100; sendGraphicPacks("open-folder"); setTimeout(updateGraphicPacksControls, 1150);
});
window.addEventListener("launcher-graphic-packs-result", event => {
  if (!graphicPacksPending || event.detail?.requestId !== graphicPacksPending) return;
  graphicPacksPending = 0;
  const state = event.detail.graphicPacks;
  const validCategory = category => category && typeof category.name === "string" && typeof category.selected === "string" && Array.isArray(category.options) && category.options.length <= 512 && category.options.every(name => typeof name === "string");
  const validPack = pack => pack && ["id","name","description"].every(key => typeof pack[key] === "string") && typeof pack.enabled === "boolean" && Array.isArray(pack.categories) && pack.categories.length <= 512 && pack.categories.every(validCategory);
  if (state && typeof state.revision === "string" && typeof state.canEdit === "boolean" && Array.isArray(state.packs) && state.packs.length <= 512 && state.packs.every(validPack)) {
    graphicPacksSnapshot = state; renderGraphicPacks();
  }
  graphicPacksNotice.textContent = graphicPacksMessage(state?.problem || state?.notice);
  updateGraphicPacksControls();
});
window.addEventListener("launcher-core-state", updateGraphicPacksView);
document.addEventListener("visibilitychange", updateGraphicPacksView);
