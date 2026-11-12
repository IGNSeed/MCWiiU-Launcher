"use strict";

const textureDialog = document.getElementById("texture-confirmation");
let textureSnapshot = null, texturePending = 0, textureSerial = Date.now(), textureOperation = "";
let textureVisible = false, textureRecovery = false, textureFocus = null, textureFolderCooldown = 0;
const textureMessages = {
  applied:"適用しました。次回の起動に反映されます。", recovered:"Default に復旧しました。", "preview-ready":"", "folder-opened":"", "folder-open-busy":"",
  "not-ready":"処理の完了後に操作してください。", "game-running":"ゲーム停止後に適用してください。", "source-changed":"パックが変更されました。Refresh して確認してください。",
  "recovery-required":"適用状態の復旧が必要です。", "selection-missing":"適用状態を確認できません。", "stale-preview":"確認の有効期限が切れました。もう一度選択してください。",
  "common-root-required":"直下に Common フォルダが必要です。", "invalid-zip":"ZIPを読み込めません。", "zip-crc":"ZIPの整合性を確認できません。", "duplicate-entry":"同じパスのファイルが重複しています。",
  "unsafe-pack-path":"パックに安全でないパスがあります。", "unsafe-path":"安全な保存先を確認できません。", "unsafe-file":"リンクされたファイルは使用できません。", "zip-link-rejected":"ZIP内のリンクは使用できません。",
  "incompatible-png-size":"v560 の画像構成と一致しません。", "incompatible-terrain":"v560 の Terrain 構成と一致しません。", "terrain-mipmaps-required":"Terrain と2段階の Mipmap が必要です。", "terrain-mipmap-size":"Terrain と Mipmap の寸法が一致しません。",
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
function textureCard(pack, badge) {
  const card = textureNode("div", undefined, "texture-card");
  const frame = textureNode("div", undefined, "texture-icon");
  const fallback = textureNode("span", undefined, "texture-icon-fallback"); fallback.setAttribute("aria-hidden","true"); frame.append(fallback);
  if (pack?.icon && /^data:image\/png;base64,[A-Za-z0-9+/=]+$/.test(pack.icon)) {
    const image = new Image(); image.alt = ""; image.src = pack.icon; image.addEventListener("error", () => image.remove(), {once:true}); frame.append(image);
  }
  const text = textureNode("div", undefined, "texture-copy");
  const name = textureNode("strong", pack?.name || "—", "texture-name"); name.title = pack?.name || "";
  const description = textureNode("p", pack?.description || "", "texture-description"); description.title = pack?.description || ""; text.append(name,description);
  if (pack?.problem) text.append(textureNode("p",textureMessage(pack.problem),"texture-problem"));
  card.append(frame,text);
  if (badge) card.append(textureNode("span",badge,"texture-badge"));
  return card;
}
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
  textureOperation = op; texturePending = ++textureSerial;
  document.getElementById("texture-notice").textContent = op === "list" ? "テクスチャを読み込み中…" : "処理中…";
  document.getElementById("texture-confirmation-notice").textContent = "";
  updateTextureControls();
  windowCommand(`texture:${op === "open-folder" ? "open-folder" : "command"}:${texturePending}:${JSON.stringify(op === "open-folder" ? {} : {op,...extra})}`);
}
function updateTextureView() {
  const visible = selectedPage === "texture" && !document.hidden;
  if (visible && !textureVisible && textureReady() && !texturePending) { textureVisible = true; sendTexture("list"); }
  else if (!visible) textureVisible = false;
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
  const controls = [...textureDialog.querySelectorAll("button")].filter(button => !button.disabled);
  if (event.shiftKey && document.activeElement === controls[0]) { event.preventDefault(); controls.at(-1)?.focus(); }
  else if (!event.shiftKey && document.activeElement === controls.at(-1)) { event.preventDefault(); controls[0]?.focus(); }
});
window.addEventListener("launcher-texture-result",event => {
  if (event.detail?.requestId !== texturePending) return;
  const op = textureOperation, state = event.detail.texture; texturePending = 0;
  const validPack = pack => pack && ["id","name","description","icon","problem","fingerprint","kind","source"].every(key => typeof pack[key] === "string") && (!pack.icon || /^data:image\/png;base64,[A-Za-z0-9+/=]+$/.test(pack.icon));
  if (state && Number.isSafeInteger(state.revision) && Array.isArray(state.packs) && state.packs.length <= 257 && validPack(state.current) && state.packs.every(validPack)) {
    textureSnapshot = state;
    if (op === "preview" && state.token && validPack(state.preview)) {
      document.getElementById("texture-confirmation-title").textContent = "Texture を適用";
      document.getElementById("texture-preview").replaceChildren(textureCard(state.preview));
      document.getElementById("texture-confirmation-count").textContent = `${state.preview.fileCount} ファイルを置換 · 次回の起動に反映されます。`;
      textureDialog.showModal(); document.getElementById("texture-cancel").focus();
    }
    if (["applied","recovered"].includes(state.notice)) textureDialog.close();
    renderTextures();
  }
  const notice = textureMessage(state?.notice);
  document.getElementById("texture-notice").textContent = notice;
  if (textureDialog.open) document.getElementById("texture-confirmation-notice").textContent = notice;
  updateTextureControls();
});
window.addEventListener("launcher-core-state",updateTextureView);
document.addEventListener("visibilitychange",updateTextureView);
