"use strict";

const textureDialog = document.getElementById("texture-confirmation");
const textureFallbackIcon = "assets/pack_icon.png";
let textureSnapshot = null, texturePending = 0, textureSerial = Date.now(), textureOperation = "";
let textureRendered = "";
let textureDirty = false, textureVisible = false, textureRecovery = false, textureFocus = null, textureFolderCooldown = 0;
const textureMessages = {
  get applied() { return LauncherI18n.t("Applied. Available on next launch."); }, get recovered() { return LauncherI18n.t("Restored defaults."); }, "preview-ready":"", "folder-opened":"", "folder-open-busy":"",
  get "warning-acknowledged"() { return LauncherI18n.t("Compatibility confirmation saved."); }, get "warning-unacknowledged"() { return LauncherI18n.t("Compatibility confirmation removed."); },
  get "acknowledgements-unavailable"() { return LauncherI18n.t("Cannot load compatibility confirmations. Showing warnings."); }, get "acknowledgements-save-failed"() { return LauncherI18n.t("Cannot save compatibility confirmation. Showing warnings."); }, get "acknowledgements-limit"() { return LauncherI18n.t("Confirmation limit reached. Remove unused confirmations."); },
  get "not-ready"() { return LauncherI18n.t("Wait for the current operation to finish."); }, get "game-running"() { return LauncherI18n.t("Apply after the game stops."); }, get "source-changed"() { return LauncherI18n.t("Pack changed. Select it again after the list refreshes."); },
  get "recovery-required"() { return LauncherI18n.t("Texture recovery required."); }, get "selection-missing"() { return LauncherI18n.t("Cannot verify applied textures."); }, get "stale-preview"() { return LauncherI18n.t("Confirmation expired. Select the pack again."); },
  get "common-root-required"() { return LauncherI18n.t("A Common folder is required at the root."); }, get "invalid-zip"() { return LauncherI18n.t("Cannot read ZIP."); }, get "zip-crc"() { return LauncherI18n.t("Cannot verify ZIP integrity."); }, get "duplicate-entry"() { return LauncherI18n.t("Duplicate file paths found."); },
  get "unsafe-pack-path"() { return LauncherI18n.t("Pack contains unsafe paths."); }, get "unsafe-path"() { return LauncherI18n.t("Cannot verify a safe destination."); }, get "unsafe-file"() { return LauncherI18n.t("Linked files are unsupported."); }, get "zip-link-rejected"() { return LauncherI18n.t("ZIP links are unsupported."); },
  get "incompatible-png-size"() { return LauncherI18n.t("Image layout does not match v560."); }, get "incompatible-terrain"() { return LauncherI18n.t("Terrain layout does not match v560."); }, get "terrain-mipmaps-required"() { return LauncherI18n.t("Terrain and two mipmap levels are required."); }, get "terrain-mipmap-size"() { return LauncherI18n.t("Terrain and mipmap dimensions do not match."); },
  get "unusual-png-size"() { return LauncherI18n.t("Unusual resolution. Check appearance in the game."); }, get "animation-layout"() { return LauncherI18n.t("Unusual animation frame count or resolution."); }, get "animation-sequence"() { return LauncherI18n.t("Animation frame order or timing has changed."); },
  get "animation-strip-size"() { return LauncherI18n.t("Stack square animation frames vertically."); }, get "animation-frame-range"() { return LauncherI18n.t("Animation references frames missing from the image."); }, get "invalid-animation-sequence"() { return LauncherI18n.t("Cannot read animation definitions."); }, get "animation-sequence-limit"() { return LauncherI18n.t("Animation definition size or frame count exceeds the limit."); },
  get "incompatible-items"() { return LauncherI18n.t("Items require a grid of 16 columns and 17 rows of square cells."); }, get "incompatible-particles"() { return LauncherI18n.t("Particles require a square image."); }, get "incompatible-sky"() { return LauncherI18n.t("Cannot verify sky image layout."); }, get "description-size-limit"() { return LauncherI18n.t("Description exceeds the size limit."); },
  get "unsupported-resource"() { return LauncherI18n.t("Some resources cannot be verified for v560."); }, get "unverified-sequence"() { return LauncherI18n.t("Modified animation definitions are unsupported."); }, get "invalid-png"() { return LauncherI18n.t("Cannot read PNG."); }, get "png-crc"() { return LauncherI18n.t("Cannot verify PNG integrity."); }, get "png-size-limit"() { return LauncherI18n.t("Image dimensions exceed the limit."); },
  get "media-not-v560"() { return LauncherI18n.t("Media layout does not match v560."); }, get "media-entry-mismatch"() { return LauncherI18n.t("Media entries do not match v560."); }, get "media-unverified-entry"() { return LauncherI18n.t("Media contains unsupported changes."); }, get "invalid-v560-hud"() { return LauncherI18n.t("Cannot verify v560 HUD."); },
  get "media-hud-prefix-changed"() { return LauncherI18n.t("HUD structure has changed."); }, get "media-hud-descriptor-changed"() { return LauncherI18n.t("HUD image definitions have changed."); }, get "media-hud-image-size"() { return LauncherI18n.t("HUD image dimensions do not match."); },
  get "snapshot-hash-mismatch"() { return LauncherI18n.t("Cannot verify applied data integrity."); }, get "manifest-hash-mismatch"() { return LauncherI18n.t("Cannot verify application record integrity."); }, get "file-busy-or-missing"() { return LauncherI18n.t("File missing or in use by another application."); },
  get "folder-open-failed"() { return LauncherI18n.t("Could not open File Explorer."); }, get "write-failed"() { return LauncherI18n.t("Could not write. Check free space and file access."); }, get "snapshot-storage-limit"() { return LauncherI18n.t("Application history size limit reached."); },
  get "pack-size-limit"() { return LauncherI18n.t("Pack size or file count exceeds the limit."); }, get "file-size-limit"() { return LauncherI18n.t("File size exceeds the limit."); }, get "zip-size-limit"() { return LauncherI18n.t("ZIP size or compression ratio exceeds the limit."); }, get "pack-count-limit"() { return LauncherI18n.t("Pack count exceeds the limit."); }, get "empty-pack"() { return LauncherI18n.t("Common contains no files."); }
};
function textureMessage(value) { return value ? textureMessages[value] ?? LauncherI18n.t("Cannot verify applied data. Wait for refresh or restore defaults.") : ""; }
function textureNode(tag, text, className) {
  const node = document.createElement(tag); if (text !== undefined) LauncherI18n.bind(node, "textContent", () => text); if (className) node.className = className; return node;
}
function textureReady() { return window.launcherCoreState?.corePhase === "ready" && !window.launcherCoreState?.stopRequested; }
function textureEditable() { return textureReady() && window.launcherCoreState?.canEditTextures === true && textureSnapshot?.canEdit === true && !texturePending; }
function textureAcknowledgementEditable() { return textureReady() && window.launcherCoreState?.canAcknowledgeTextureWarnings === true && textureSnapshot?.canAcknowledge === true && !texturePending && !textureDialog.open; }
function textureCard(pack, badge, showDetails = false) {
  const card = textureNode("div", undefined, "texture-card");
  const frame = textureNode("div", undefined, "texture-icon");
  const fallback = textureNode("span", undefined, "texture-icon-fallback"); fallback.setAttribute("aria-hidden","true"); frame.append(fallback);
  const customIcon = pack?.icon && /^data:image\/png;base64,[A-Za-z0-9+/=]+$/.test(pack.icon);
  const image = new Image(); LauncherI18n.bind(image, "alt", () => "");
  let usingFallback = !customIcon;
  image.addEventListener("error", () => {
    if (!usingFallback) { usingFallback = true; image.src = textureFallbackIcon; }
    else image.remove();
  });
  image.src = customIcon ? pack.icon : textureFallbackIcon; frame.append(image);
  const text = textureNode("div", undefined, "texture-copy");
  const name = textureNode("strong", pack?.name || "—", "texture-name"); LauncherI18n.bind(name, "title", () => pack?.name || "");
  const description = textureNode("p", pack?.description || "", "texture-description"); LauncherI18n.bind(description, "title", () => pack?.description || ""); text.append(name,description);
  if (pack?.problem) text.append(textureNode("p",`Error · ${textureMessage(pack.problem)}`,"texture-problem"));
  const acknowledged = pack?.warningAcknowledged === true && pack.status === "warning" && !pack.problem;
  if (acknowledged) text.append(textureNode("p",LauncherI18n.t("Confirmed by user"),"texture-acknowledged"));
  if (pack?.issues?.length && !acknowledged) {
    const details = textureNode("details",undefined,"texture-diagnostics"); details.open = showDetails;
    const warning = pack.status === "warning";
    details.append(textureNode("summary",warning ? LauncherI18n.t("Warning · Check appearance in the game ({0})", [pack.issueCount]) : LauncherI18n.t("Details ({0})", [pack.issueCount])));
    const list = textureNode("ul");
    for (const issue of pack.issues) {
      const item = textureNode("li");
      item.append(textureNode("strong",`${issue.severity === "warning" ? LauncherI18n.t("Warning") : LauncherI18n.t("Error")} · ${issue.path}`));
      item.append(textureNode("p",textureMessage(issue.code)));
      if (issue.width && issue.height) item.append(textureNode("p",LauncherI18n.t("Actual {0}×{1}{2}", [issue.width, issue.height, issue.defaultWidth && issue.defaultHeight ? ` · Default ${issue.defaultWidth}×${issue.defaultHeight}` : ""])));
      list.append(item);
    }
    details.append(list);
    if (pack.issueCount > pack.issues.length) details.append(textureNode("p",LauncherI18n.t("{0} more. Check the affected files.", [pack.issueCount - pack.issues.length])));
    text.append(details);
  }
  card.append(frame,text);
  if (badge) card.append(textureNode("span",badge,"texture-badge"));
  if (!showDetails && pack?.status === "warning" && !pack.problem && pack.warningSignature) { card.dataset.texturePack = pack.id; card.tabIndex = 0; card.setAttribute("aria-haspopup","menu"); LauncherI18n.bind(card, "aria-label", () => LauncherI18n.t("{0} · Texture pack", [pack.name]), true); }
  return card;
}
const textureMenu = document.getElementById("texture-menu"), textureMenuConfirm = document.getElementById("texture-menu-confirm");
let textureMenuTarget = null, textureMenuFocus = null;
function focusTexturePack(target) {
  if (!target || selectedPage !== "texture") return;
  const container = document.getElementById(target.current ? "texture-current" : "texture-list");
  const card = [...container.querySelectorAll("[data-texture-pack]")].find(node => node.dataset.texturePack === target.id);
  (card || document.getElementById("texture-open-folder")).focus({preventScroll:true});
}
function closeTextureMenu(restore = false) {
  const target = textureMenuTarget; textureMenuTarget = null; textureMenu.hidePopover();
  if (restore) focusTexturePack(target);
}
function textureMenuPack(target) {
  if (!target || !textureAcknowledgementEditable() || selectedPage !== "texture" || target.revision !== textureSnapshot.revision || !target.opener.isConnected) return null;
  if (target.current && textureSnapshot.state !== "known-pack") return null;
  const pack = target.current ? textureSnapshot.current : textureSnapshot.packs.find(pack => pack.id === target.id);
  return pack?.status === "warning" && !pack.problem && pack.id === target.id && pack.fingerprint === target.fingerprint && pack.warningSignature === target.warningSignature ? pack : null;
}
function openTextureMenu(event, keyboard = false) {
  const card = event.target.closest("[data-texture-pack]");
  if (!card) return;
  const current = !!card.closest("#texture-current");
  const pack = current ? textureSnapshot?.current : textureSnapshot?.packs.find(pack => pack.id === card.dataset.texturePack);
  if (!pack) return;
  const target = {id:pack.id,fingerprint:pack.fingerprint,warningSignature:pack.warningSignature,revision:textureSnapshot.revision,current,opener:card};
  if (!textureMenuPack(target)) return;
  event.preventDefault(); closeTextureMenu(); textureMenuTarget = target;
  LauncherI18n.bind(textureMenuConfirm, "textContent", () => pack.warningAcknowledged ? LauncherI18n.t("Remove confirmation") : LauncherI18n.t("✓ Confirm working correctly"));
  textureMenu.showPopover();
  const rect = card.getBoundingClientRect(), x = keyboard ? rect.left : event.clientX, y = keyboard ? rect.bottom : event.clientY;
  textureMenu.style.left = `${Math.max(8,Math.min(innerWidth - textureMenu.offsetWidth - 8,x))}px`;
  textureMenu.style.top = `${Math.max(8,Math.min(innerHeight - textureMenu.offsetHeight - 8,y))}px`;
  textureMenuConfirm.focus({preventScroll:true});
}
document.getElementById("texture").addEventListener("contextmenu",event => openTextureMenu(event));
document.getElementById("texture").addEventListener("keydown",event => { if (event.key === "ContextMenu" || (event.shiftKey && event.key === "F10")) openTextureMenu(event,true); });
textureMenuConfirm.addEventListener("click",() => {
  const target = textureMenuTarget, pack = textureMenuPack(target); closeTextureMenu();
  if (!pack) return;
  textureMenuFocus = target; sendTexture(pack.warningAcknowledged ? "unacknowledge" : "acknowledge",{id:pack.id,fingerprint:pack.fingerprint,warningSignature:pack.warningSignature,revision:target.revision});
});
textureMenu.addEventListener("toggle",event => { if (event.newState === "closed" && textureMenuTarget) closeTextureMenu(true); });
textureMenu.addEventListener("keydown",event => { if (["ArrowDown","ArrowUp","Home","End"].includes(event.key)) { event.preventDefault(); textureMenuConfirm.focus(); } else if (event.key === "Tab") closeTextureMenu(true); });
window.addEventListener("resize",() => closeTextureMenu(true));
function updateTextureControls() {
  const ready = textureReady(), busy = !!texturePending || window.launcherCoreState?.textureBusy === true;
  document.getElementById("texture-open-folder").disabled = !ready || busy || performance.now() < textureFolderCooldown;
  document.getElementById("texture-recover").disabled = busy || window.launcherCoreState?.canEditTextures !== true;
  document.getElementById("texture-apply").disabled = busy || window.launcherCoreState?.canEditTextures !== true || (!textureRecovery && !textureSnapshot?.token);
  document.getElementById("texture-cancel").disabled = busy;
  for (const button of document.querySelectorAll("[data-texture-id]")) button.disabled = !textureEditable() || button.dataset.invalid === "true";
  document.getElementById("texture-list").setAttribute("aria-busy",String(busy));
}
function renderTextures() {
  const key = JSON.stringify([textureSnapshot?.state,textureSnapshot?.current,textureSnapshot?.packs,textureSnapshot?.running,textureSnapshot?.loadedName]);
  if (key === textureRendered) { updateTextureControls(); return; }
  textureRendered = key;
  closeTextureMenu();
  const state = textureSnapshot;
  const badges = {get "default"() { return LauncherI18n.t("Default"); }, get "known-pack"() { return LauncherI18n.t("Active"); }, get "external-unknown"() { return LauncherI18n.t("External / Unknown"); }, get error() { return LauncherI18n.t("Error"); }, get "recovery-required"() { return LauncherI18n.t("Recovery Required"); }};
  document.getElementById("texture-current").replaceChildren(textureCard(state?.current,badges[state?.state] || "—"));
  const rows = document.createDocumentFragment();
  for (const pack of state?.packs || []) {
    const active = ["default","known-pack"].includes(state.state) && state.current.id === pack.id && (pack.id === "default" || state.current.fingerprint === pack.fingerprint);
    const card = textureCard(pack,active ? LauncherI18n.t("Active") : ""); LauncherI18n.bind(card, "title", () => pack.kind === "zip" ? `ZIP · ${pack.source}` : pack.kind === "folder" ? `Folder · ${pack.source}` : "");
    if (!active) {
      const button = textureNode("button",LauncherI18n.t("Apply")); button.type = "button"; button.dataset.textureId = pack.id; button.dataset.invalid = String(!!pack.problem);
      LauncherI18n.bind(button, "aria-label", () => LauncherI18n.t("Apply {0}", [pack.name]), true); button.addEventListener("click",() => { if (textureEditable() && !pack.problem) { textureFocus = button; textureRecovery = false; sendTexture("preview",{id:pack.id,revision:textureSnapshot.revision}); } }); card.append(button);
    }
    rows.append(card);
  }
  document.getElementById("texture-list").replaceChildren(rows);
  document.getElementById("texture-recover").hidden = !state || ["default","known-pack"].includes(state.state);
  const loaded = document.getElementById("texture-loaded"); loaded.hidden = !state?.running; LauncherI18n.bind(loaded, "textContent", () => state?.running ? LauncherI18n.t("Running: {0} · Apply after the game stops.", [state.loadedName]) : "");
  updateTextureControls();
}
function sendTexture(op, extra = {}) {
  if (texturePending || !textureReady()) return;
  closeTextureMenu();
  textureOperation = op; texturePending = ++textureSerial;
  LauncherI18n.bind(document.getElementById("texture-notice"), "textContent", () => op === "list" ? LauncherI18n.t("Loading textures…") : LauncherI18n.t("Working…"));
  LauncherI18n.bind(document.getElementById("texture-confirmation-notice"), "textContent", () => "");
  updateTextureControls();
  windowCommand(`texture:${op === "open-folder" ? "open-folder" : "command"}:${texturePending}:${JSON.stringify(op === "open-folder" ? {} : {op,...extra})}`);
}
function updateTextureView() {
  const visible = selectedPage === "texture" && !document.hidden;
  if (visible && (!textureVisible || textureDirty) && textureReady() && !texturePending && !textureDialog.open && !textureMenu.matches(":popover-open")) { textureVisible = true; textureDirty = false; sendTexture("list"); }
  else if (!visible) { textureVisible = false; closeTextureMenu(); textureMenuFocus = null; }
  if (!textureAcknowledgementEditable()) closeTextureMenu();
  if (visible && !textureReady()) LauncherI18n.bind(document.getElementById("texture-notice"), "textContent", () => window.launcherCoreState?.phase === "unavailable" ? LauncherI18n.t("Texture management requires Cemu Core.") : LauncherI18n.t("Preparing…"));
  updateTextureControls();
}
document.getElementById("texture-open-folder").addEventListener("click",() => { textureFolderCooldown = performance.now() + 1100; sendTexture("open-folder"); setTimeout(updateTextureControls,1150); });
document.getElementById("texture-recover").addEventListener("click",() => {
  textureRecovery = true; textureFocus = document.getElementById("texture-recover");
  LauncherI18n.bind(document.getElementById("texture-confirmation-title"), "textContent", () => LauncherI18n.t("Restore defaults"));
  document.getElementById("texture-preview").replaceChildren(textureCard({get name() { return LauncherI18n.t("Default"); },get description() { return LauncherI18n.t("Original Minecraft textures"); }}));
  LauncherI18n.bind(document.getElementById("texture-confirmation-count"), "textContent", () => LauncherI18n.t("Remove texture replacements? Source packs and application history are preserved."));
  LauncherI18n.bind(document.getElementById("texture-confirmation-notice"), "textContent", () => ""); textureDialog.showModal(); updateTextureControls(); document.getElementById("texture-cancel").focus();
});
document.getElementById("texture-apply").addEventListener("click",() => { if (!document.getElementById("texture-apply").disabled) sendTexture(textureRecovery ? "recover" : "apply",textureRecovery ? {} : {token:textureSnapshot.token}); });
function cancelTexture() { if (texturePending) return; textureDialog.close(); if (!textureRecovery) sendTexture("cancel"); }
document.getElementById("texture-cancel").addEventListener("click",cancelTexture);
textureDialog.addEventListener("cancel",event => { event.preventDefault(); cancelTexture(); });
textureDialog.addEventListener("close",() => { if (textureFocus?.isConnected) textureFocus.focus({preventScroll:true}); updateTextureControls(); });
textureDialog.addEventListener("keydown",event => {
  if (event.key !== "Tab") return;
  const controls = [...textureDialog.querySelectorAll("button, summary")].filter(control => !control.disabled);
  if (event.shiftKey && document.activeElement === controls[0]) { event.preventDefault(); controls.at(-1)?.focus(); }
  else if (!event.shiftKey && document.activeElement === controls.at(-1)) { event.preventDefault(); controls[0]?.focus(); }
});
window.addEventListener("launcher-texture-result",event => {
  if (event.detail?.requestId !== texturePending) return;
  const op = textureOperation, state = event.detail.texture; texturePending = 0;
  const validIssue = issue => issue && ["warning","error"].includes(issue.severity) && typeof issue.path === "string" && typeof issue.code === "string" && ["width","height","defaultWidth","defaultHeight"].every(key => Number.isSafeInteger(issue[key]) && issue[key] >= 0 && issue[key] <= 8192);
  const validPack = pack => pack && ["id","name","description","icon","problem","fingerprint","kind","source","warningSignature"].every(key => typeof pack[key] === "string") && typeof pack.warningAcknowledged === "boolean" && (!pack.warningSignature || /^[a-f0-9]{64}$/.test(pack.warningSignature)) && (!pack.icon || /^data:image\/png;base64,[A-Za-z0-9+/=]+$/.test(pack.icon)) && ["valid","warning","error"].includes(pack.status) && Array.isArray(pack.issues) && pack.issues.length <= 32 && pack.issues.every(validIssue) && Number.isSafeInteger(pack.issueCount) && pack.issueCount >= pack.issues.length;
  if (state && Number.isSafeInteger(state.revision) && Array.isArray(state.packs) && state.packs.length <= 257 && validPack(state.current) && state.packs.every(validPack)) {
    textureSnapshot = state;
    if (op === "preview" && state.token && validPack(state.preview)) {
      LauncherI18n.bind(document.getElementById("texture-confirmation-title"), "textContent", () => LauncherI18n.t("Apply texture"));
      document.getElementById("texture-preview").replaceChildren(textureCard(state.preview,undefined,true));
      LauncherI18n.bind(document.getElementById("texture-confirmation-count"), "textContent", () => LauncherI18n.t("Replace {0} files · Applies on next launch.", [state.preview.fileCount]));
      textureDialog.showModal(); document.getElementById("texture-cancel").focus();
    }
    if (["applied","recovered"].includes(state.notice)) textureDialog.close();
    renderTextures();
    if (textureMenuFocus) { focusTexturePack(textureMenuFocus); textureMenuFocus = null; }
  }
  LauncherI18n.bind(document.getElementById("texture-notice"), "textContent", () => textureMessage(state?.notice || state?.acknowledgementNotice));
  if (textureDialog.open) LauncherI18n.bind(document.getElementById("texture-confirmation-notice"), "textContent", () => textureMessage(state?.notice || state?.acknowledgementNotice));
  updateTextureControls();
});
let textureManagerGeneration;
window.addEventListener("launcher-core-state", () => {
  const generation = window.launcherCoreState?.managerGeneration;
  if (generation !== textureManagerGeneration) {
    textureManagerGeneration = generation; textureSnapshot = null; texturePending = 0; textureOperation = ""; textureVisible = false;
    textureDialog.close(); closeTextureMenu(); renderTextures();
  }
  updateTextureView();
});
document.addEventListener("visibilitychange",updateTextureView);

window.addEventListener("launcher-library-change", event => {
  if (event.detail?.name !== "texture") return;
  textureDirty = true; updateTextureView();
});
window.addEventListener("launcher-live-state", () => { if (selectedPage === "texture" && !document.hidden) updateTextureView(); });

window.addEventListener("launcher-language-changed", () => { textureRendered = ""; renderTextures(); });
