"use strict";

let graphicPacksSnapshot = null, graphicPacksPending = 0, graphicPacksSerial = 0;
let graphicPacksRendered = "";
let graphicPacksDirty = false, graphicPacksVisible = false, graphicPacksCooldown = 0, graphicPacksContext = "";
const graphicPacksList = document.getElementById("graphic-packs-list");
const graphicPacksNotice = document.getElementById("graphic-packs-notice");
const graphicPacksFolder = document.getElementById("graphic-packs-open-folder");
function graphicPacksReady() { return window.launcherCoreState?.corePhase === "ready"; }
function graphicPacksEditable() {
  return !graphicPacksPending && window.launcherCoreState?.canEditGraphicPacks === true && graphicPacksSnapshot?.canEdit === true;
}
function graphicPacksMessage(value) {
  const messages = {
    get saved() { return LauncherI18n.t("Saved."); },
    get "game-running-or-busy"() { return LauncherI18n.t("Available after the game stops."); },
    get "stale-revision"() { return LauncherI18n.t("Settings changed. Reopen the list."); },
    get "stale-request"() { return LauncherI18n.t("Stale request ignored."); },
    get "folder-opened"() { return ""; },
    get "folder-open-busy"() { return LauncherI18n.t("Opening folder…"); },
    get "invalid-pack-skipped"() { return LauncherI18n.t("Some packs could not be read. Check rules.txt; the list refreshes automatically."); },
    get "settings-corrupt"() { return LauncherI18n.t("Cannot read settings. Existing settings were not overwritten."); },
    get "pack-changed"() { return LauncherI18n.t("Pack changed. Select it again after the list refreshes."); },
    get "save-failed"() { return LauncherI18n.t("Could not save. Previous selections are preserved."); },
    get "invalid-preset"() { return LauncherI18n.t("This preset is unavailable."); },
    get "invalid-request"() { return LauncherI18n.t("Request rejected."); },
    get "not-ready"() { return LauncherI18n.t("Graphic pack management unavailable."); }
  };
  return messages[value] || (value ? LauncherI18n.t("Could not complete the action. Check settings and packs.") : "");
}
function updateGraphicPacksControls() {
  graphicPacksFolder.disabled = !graphicPacksReady() || !!graphicPacksPending || performance.now() < graphicPacksCooldown;
  for (const input of graphicPacksList.querySelectorAll("input, select")) input.disabled = !graphicPacksEditable();
  graphicPacksList.setAttribute("aria-busy", String(!!graphicPacksPending));
}
function renderGraphicPacks() {
  const key = LauncherI18n.language + JSON.stringify(graphicPacksSnapshot.packs);
  if (key === graphicPacksRendered) { updateGraphicPacksControls(); return; }
  graphicPacksRendered = key;
  const focus = document.activeElement;
  const focusId = focus?.dataset.graphicPackId, focusCategory = focus?.dataset.graphicPackCategory;
  graphicPacksList.replaceChildren();
  const packs = graphicPacksSnapshot.packs;
  if (!packs.length) {
    const empty = document.createElement("p"); empty.className = "footnote";
    LauncherI18n.bind(empty, "textContent", () => LauncherI18n.t("No Minecraft graphic packs installed."));
    graphicPacksList.append(empty);
  }
  for (const pack of packs) {
    const card = document.createElement("article"); card.className = "graphic-pack";
    const row = document.createElement("label"); row.className = "graphic-pack-title";
    const toggle = document.createElement("input"); toggle.type = "checkbox"; toggle.checked = pack.enabled;
    toggle.dataset.graphicPackId = pack.id;
    const name = document.createElement("span"); LauncherI18n.bind(name, "textContent", () => pack.name);
    const status = document.createElement("span"); status.className = "footnote"; LauncherI18n.bind(status, "textContent", () => pack.enabled ? LauncherI18n.t("Enabled") : LauncherI18n.t("Disabled"));
    row.append(toggle, name, status); card.append(row);
    toggle.addEventListener("change", () => {
      const enabled = toggle.checked; toggle.checked = pack.enabled;
      if (graphicPacksEditable()) sendGraphicPacks("enable", {id:pack.id, enabled, revision:graphicPacksSnapshot.revision});
    });
    if (pack.description) { const description = document.createElement("p"); description.className = "graphic-pack-description footnote"; LauncherI18n.bind(description, "textContent", () => pack.description); card.append(description); }
    for (const category of pack.categories) {
      if (!category.options.length) continue;
      const label = document.createElement("label"); label.className = "graphic-pack-preset input-row";
      const title = document.createElement("span"); LauncherI18n.bind(title, "textContent", () => category.name || LauncherI18n.t("Preset"));
      const select = document.createElement("select"); select.dataset.graphicPackId = pack.id; select.dataset.graphicPackCategory = category.name;
      for (const option of category.options) { const node = document.createElement("option"); node.value = option; LauncherI18n.bind(node, "textContent", () => option); select.append(node); }
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
  LauncherI18n.bind(graphicPacksNotice, "textContent", () => op === "list" ? LauncherI18n.t("Loading graphic packs…") : LauncherI18n.t("Working…"));
  updateGraphicPacksControls();
  windowCommand(`graphic-packs:${op === "open-folder" ? "open-folder" : "command"}:${graphicPacksPending}:${JSON.stringify(op === "open-folder" ? {} : {op,...extra})}`);
}
function updateGraphicPacksView() {
  const visible = selectedPage === "graphic-packs" && !document.hidden;
  const context = `${window.launcherCoreState?.phase}|${window.launcherCoreState?.canEditGraphicPacks}`;
  if (visible && (!graphicPacksVisible || graphicPacksDirty || context !== graphicPacksContext) && graphicPacksReady() && !graphicPacksPending) {
    graphicPacksVisible = true; graphicPacksDirty = false; graphicPacksContext = context; sendGraphicPacks("list");
  } else if (!visible) graphicPacksVisible = false;
  if (visible && !graphicPacksReady()) LauncherI18n.bind(graphicPacksNotice, "textContent", () => window.launcherCoreState?.phase === "unavailable" ? LauncherI18n.t("Graphic pack management requires Cemu Core.") : LauncherI18n.t("Preparing…"));
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
  LauncherI18n.bind(graphicPacksNotice, "textContent", () => graphicPacksMessage(state?.problem || state?.notice));
  updateGraphicPacksControls();
});
window.addEventListener("launcher-core-state", updateGraphicPacksView);
document.addEventListener("visibilitychange", updateGraphicPacksView);

window.addEventListener("launcher-library-change", event => {
  if (event.detail?.name !== "graphicPacks") return;
  graphicPacksDirty = true; updateGraphicPacksView();
});
window.addEventListener("launcher-live-state", event => {
  if (selectedPage !== "graphic-packs" || document.hidden) return;
  updateGraphicPacksView();
  if (event.detail?.state?.graphicPacksDeferred && window.launcherCoreState?.phase === "running") LauncherI18n.bind(graphicPacksNotice, "textContent", () => LauncherI18n.t("Folder changes will be checked after the game stops. Current selections are preserved."));
});

window.addEventListener("launcher-language-changed", () => { graphicPacksRendered = ""; if (graphicPacksSnapshot) renderGraphicPacks(); });
