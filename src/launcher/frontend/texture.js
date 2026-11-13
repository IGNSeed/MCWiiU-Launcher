"use strict";

const textureDialog = document.getElementById("texture-confirmation");
const textureFallbackIcon = "assets/pack_icon.png";
let textureSnapshot = null, texturePending = 0, textureSerial = Date.now(), textureOperation = "";
let textureVisible = false, textureRecovery = false, textureFocus = null, textureFolderCooldown = 0;
const textureMessages = {
  applied:"適用しました。次回の起動に反映されます。", recovered:"Default に復旧しました。", "preview-ready":"", "folder-opened":"", "folder-open-busy":"",
  "warning-acknowledged":"この内容の動作確認を記録しました。", "warning-unacknowledged":"確認済みを解除しました。",
  "acknowledgements-unavailable":"確認情報を読み込めません。互換性の警告を表示しています。", "acknowledgements-save-failed":"確認情報を保存できませんでした。互換性の警告を表示しています。", "acknowledgements-limit":"確認情報の保存件数が上限に達しました。不要な確認を解除してください。",
  "not-ready":"処理の完了後に操作してください。", "game-running":"ゲーム停止後に適用してください。", "source-changed":"パックが変更されました。Refresh して確認してください。",
  "recovery-required":"適用状態の復旧が必要です。", "selection-missing":"適用状態を確認できません。", "stale-preview":"確認の有効期限が切れました。もう一度選択してください。",
  "common-root-required":"直下に Common フォルダが必要です。", "invalid-zip":"ZIPを読み込めません。", "zip-crc":"ZIPの整合性を確認できません。", "duplicate-entry":"同じパスのファイルが重複しています。",
  "unsafe-pack-path":"パックに安全でないパスがあります。", "unsafe-path":"安全な保存先を確認できません。", "unsafe-file":"リンクされたファイルは使用できません。", "zip-link-rejected":"ZIP内のリンクは使用できません。",
  "incompatible-png-size":"v560 の画像構成と一致しません。", "incompatible-terrain":"v560 の Terrain 構成と一致しません。", "terrain-mipmaps-required":"Terrain と2段階の Mipmap が必要です。", "terrain-mipmap-size":"Terrain と Mipmap の寸法が一致しません。",
  "unusual-png-size":"通常とは異なる解像度です。ゲーム内での表示を確認してください。", "animation-layout":"アニメーションのフレーム数または解像度が通常と異なります。", "animation-sequence":"アニメーションの再生順序または時間が変更されています。",
  "animation-strip-size":"アニメーション画像は正方形フレームを縦に並べてください。", "animation-frame-range":"アニメーション定義が画像に存在しないフレームを参照しています。", "invalid-animation-sequence":"アニメーション定義を読み込めません。", "animation-sequence-limit":"アニメーション定義の容量・再生数が上限を超えています。",
  "incompatible-items":"Items は横16列・縦17行の正方形セルが必要です。", "incompatible-particles":"Particles は正方形の画像が必要です。", "incompatible-sky":"Sky の画像構成を確認できません。", "description-size-limit":"説明文の容量が上限を超えています。",
  "unsupported-resource":"v560 への対応を確認できないリソースがあります。", "unverified-sequence":"変更されたアニメーション定義には未対応です。", "invalid-png":"PNGを読み込めません。", "png-crc":"PNGの整合性を確認できません。", "png-size-limit":"画像のサイズが上限を超えています。",
  "media-not-v560":"Media が v560 の構成と一致しません。", "media-entry-mismatch":"Media のエントリーが v560 と一致しません。", "media-unverified-entry":"Media に未対応の変更があります。", "invalid-v560-hud":"v560 の HUD を検証できません。",
  "media-hud-prefix-changed":"HUD の構造が変更されています。", "media-hud-descriptor-changed":"HUD の画像定義が変更されています。", "media-hud-image-size":"HUD の画像サイズが一致しません。",
  "snapshot-hash-mismatch":"適用データの整合性を確認できません。", "manifest-hash-mismatch":"適用情報の整合性を確認できません。", "file-busy-or-missing":"ファイルが見つからないか、ほかのアプリで使用中です。",
  "folder-open-failed":"エクスプローラーを開けませんでした。", "write-failed":"書き込めません。空き容量と使用状況を確認してください。", "snapshot-storage-limit":"適用履歴の容量が上限に達しました。",
  "pack-size-limit":"パックの容量またはファイル数が上限を超えています。", "file-size-limit":"ファイルの容量が上限を超えています。", "zip-size-limit":"ZIPの容量または圧縮率が上限を超えています。", "pack-count-limit":"パック数が上限を超えています。", "empty-pack":"Common にファイルがありません。"
};
function textureMessage(value) { return value ? textureMessages[value] ?? "適用データを検証できません。Refresh または Default への復旧を行ってください。" : ""; }
function textureNode(tag, text, className) {
  const node = document.createElement(tag); if (text !== undefined) node.textContent = text; if (className) node.className = className; return node;
}
function textureReady() { return window.launcherCoreState?.corePhase === "ready" && !window.launcherCoreState?.stopRequested; }
function textureEditable() { return textureReady() && window.launcherCoreState?.canEditTextures === true && textureSnapshot?.canEdit === true && !texturePending; }
function textureAcknowledgementEditable() { return textureReady() && window.launcherCoreState?.canAcknowledgeTextureWarnings === true && textureSnapshot?.canAcknowledge === true && !texturePending && !textureDialog.open; }
function textureCard(pack, badge, showDetails = false) {
  const card = textureNode("div", undefined, "texture-card");
  const frame = textureNode("div", undefined, "texture-icon");
  const fallback = textureNode("span", undefined, "texture-icon-fallback"); fallback.setAttribute("aria-hidden","true"); frame.append(fallback);
  const customIcon = pack?.icon && /^data:image\/png;base64,[A-Za-z0-9+/=]+$/.test(pack.icon);
  const image = new Image(); image.alt = "";
  let usingFallback = !customIcon;
  image.addEventListener("error", () => {
    if (!usingFallback) { usingFallback = true; image.src = textureFallbackIcon; }
    else image.remove();
  });
  image.src = customIcon ? pack.icon : textureFallbackIcon; frame.append(image);
  const text = textureNode("div", undefined, "texture-copy");
  const name = textureNode("strong", pack?.name || "—", "texture-name"); name.title = pack?.name || "";
  const description = textureNode("p", pack?.description || "", "texture-description"); description.title = pack?.description || ""; text.append(name,description);
  if (pack?.problem) text.append(textureNode("p",`Error · ${textureMessage(pack.problem)}`,"texture-problem"));
  const acknowledged = pack?.warningAcknowledged === true && pack.status === "warning" && !pack.problem;
  if (acknowledged) text.append(textureNode("p","ユーザー確認済み","texture-acknowledged"));
  if (pack?.issues?.length && !acknowledged) {
    const details = textureNode("details",undefined,"texture-diagnostics"); details.open = showDetails;
    const warning = pack.status === "warning";
    details.append(textureNode("summary",warning ? `Warning · ゲーム内での表示を確認してください (${pack.issueCount})` : `詳細 (${pack.issueCount})`));
    const list = textureNode("ul");
    for (const issue of pack.issues) {
      const item = textureNode("li");
      item.append(textureNode("strong",`${issue.severity === "warning" ? "Warning" : "Error"} · ${issue.path}`));
      item.append(textureNode("p",textureMessage(issue.code)));
      if (issue.width && issue.height) item.append(textureNode("p",`実際 ${issue.width}×${issue.height}${issue.defaultWidth && issue.defaultHeight ? ` · Default ${issue.defaultWidth}×${issue.defaultHeight}` : ""}`));
      list.append(item);
    }
    details.append(list);
    if (pack.issueCount > pack.issues.length) details.append(textureNode("p",`ほか ${pack.issueCount - pack.issues.length} 件。対象ファイルを確認してください。`));
    text.append(details);
  }
  card.append(frame,text);
  if (badge) card.append(textureNode("span",badge,"texture-badge"));
  if (!showDetails && pack?.status === "warning" && !pack.problem && pack.warningSignature) { card.dataset.texturePack = pack.id; card.tabIndex = 0; card.setAttribute("aria-haspopup","menu"); card.setAttribute("aria-label",`${pack.name} · テクスチャパック`); }
  return card;
}
const textureMenu = document.getElementById("texture-menu"), textureMenuConfirm = document.getElementById("texture-menu-confirm");
let textureMenuTarget = null, textureMenuFocus = null;
function focusTexturePack(target) {
  if (!target || selectedPage !== "texture") return;
  const container = document.getElementById(target.current ? "texture-current" : "texture-list");
  const card = [...container.querySelectorAll("[data-texture-pack]")].find(node => node.dataset.texturePack === target.id);
  (card || document.getElementById("texture-refresh")).focus({preventScroll:true});
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
  textureMenuConfirm.textContent = pack.warningAcknowledged ? "確認済みを解除" : "✓ 問題なく動作することを確認";
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
  document.getElementById("texture-refresh").disabled = !ready || busy || textureDialog.open;
  document.getElementById("texture-open-folder").disabled = !ready || busy || performance.now() < textureFolderCooldown;
  document.getElementById("texture-recover").disabled = busy || window.launcherCoreState?.canEditTextures !== true;
  document.getElementById("texture-apply").disabled = busy || window.launcherCoreState?.canEditTextures !== true || (!textureRecovery && !textureSnapshot?.token);
  document.getElementById("texture-cancel").disabled = busy;
  for (const button of document.querySelectorAll("[data-texture-id]")) button.disabled = !textureEditable() || button.dataset.invalid === "true";
  document.getElementById("texture-list").setAttribute("aria-busy",String(busy));
}
function renderTextures() {
  closeTextureMenu();
  const state = textureSnapshot;
  const badges = {"default":"Default", "known-pack":"Active", "external-unknown":"External / Unknown", error:"Error", "recovery-required":"Recovery Required"};
  document.getElementById("texture-current").replaceChildren(textureCard(state?.current,badges[state?.state] || "—"));
  const rows = document.createDocumentFragment();
  for (const pack of state?.packs || []) {
    const active = ["default","known-pack"].includes(state.state) && state.current.id === pack.id && (pack.id === "default" || state.current.fingerprint === pack.fingerprint);
    const card = textureCard(pack,active ? "Active" : ""); card.title = pack.kind === "zip" ? `ZIP · ${pack.source}` : pack.kind === "folder" ? `Folder · ${pack.source}` : "";
    if (!active) {
      const button = textureNode("button","Apply"); button.type = "button"; button.dataset.textureId = pack.id; button.dataset.invalid = String(!!pack.problem);
      button.setAttribute("aria-label",`Apply ${pack.name}`); button.addEventListener("click",() => { if (textureEditable() && !pack.problem) { textureFocus = button; textureRecovery = false; sendTexture("preview",{id:pack.id,revision:state.revision}); } }); card.append(button);
    }
    rows.append(card);
  }
  document.getElementById("texture-list").replaceChildren(rows);
  document.getElementById("texture-recover").hidden = !state || ["default","known-pack"].includes(state.state);
  const loaded = document.getElementById("texture-loaded"); loaded.hidden = !state?.running; loaded.textContent = state?.running ? `実行中：${state.loadedName} · 適用はゲーム停止後に行えます。` : "";
  updateTextureControls();
}
function sendTexture(op, extra = {}) {
  if (texturePending || !textureReady()) return;
  closeTextureMenu();
  textureOperation = op; texturePending = ++textureSerial;
  document.getElementById("texture-notice").textContent = op === "list" ? "テクスチャを読み込み中…" : "処理中…";
  document.getElementById("texture-confirmation-notice").textContent = "";
  updateTextureControls();
  windowCommand(`texture:${op === "open-folder" ? "open-folder" : "command"}:${texturePending}:${JSON.stringify(op === "open-folder" ? {} : {op,...extra})}`);
}
function updateTextureView() {
  const visible = selectedPage === "texture" && !document.hidden;
  if (visible && !textureVisible && textureReady() && !texturePending) { textureVisible = true; sendTexture("list"); }
  else if (!visible) { textureVisible = false; closeTextureMenu(); textureMenuFocus = null; }
  if (!textureAcknowledgementEditable()) closeTextureMenu();
  if (visible && !textureReady()) document.getElementById("texture-notice").textContent = window.launcherCoreState?.phase === "unavailable" ? "Core がないため Texture の管理は利用できません。" : "準備完了後にテクスチャを表示します。";
  updateTextureControls();
}
document.getElementById("texture-refresh").addEventListener("click",() => sendTexture("list"));
document.getElementById("texture-open-folder").addEventListener("click",() => { textureFolderCooldown = performance.now() + 1100; sendTexture("open-folder"); setTimeout(updateTextureControls,1150); });
document.getElementById("texture-recover").addEventListener("click",() => {
  textureRecovery = true; textureFocus = document.getElementById("texture-recover");
  document.getElementById("texture-confirmation-title").textContent = "Default に復旧";
  document.getElementById("texture-preview").replaceChildren(textureCard({name:"Default",description:"Original Minecraft textures"}));
  document.getElementById("texture-confirmation-count").textContent = "テクスチャ置換を解除します。パック原本と適用履歴は保持されます。";
  document.getElementById("texture-confirmation-notice").textContent = ""; textureDialog.showModal(); updateTextureControls(); document.getElementById("texture-cancel").focus();
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
      document.getElementById("texture-confirmation-title").textContent = "Texture を適用";
      document.getElementById("texture-preview").replaceChildren(textureCard(state.preview,undefined,true));
      document.getElementById("texture-confirmation-count").textContent = `${state.preview.fileCount} ファイルを置換 · 次回の起動に反映されます。`;
      textureDialog.showModal(); document.getElementById("texture-cancel").focus();
    }
    if (["applied","recovered"].includes(state.notice)) textureDialog.close();
    renderTextures();
    if (textureMenuFocus) { focusTexturePack(textureMenuFocus); textureMenuFocus = null; }
  }
  const notice = textureMessage(state?.notice || state?.acknowledgementNotice);
  document.getElementById("texture-notice").textContent = notice;
  if (textureDialog.open) document.getElementById("texture-confirmation-notice").textContent = notice;
  updateTextureControls();
});
window.addEventListener("launcher-core-state",updateTextureView);
document.addEventListener("visibilitychange",updateTextureView);
