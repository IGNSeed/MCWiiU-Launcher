"use strict";

const pages = ["home", "settings"];
const navigation = document.querySelectorAll("[data-page]");
function showPage(selected) {
  if (!pages.includes(selected)) return;
  stopInputView();
  hideAccountDetails();
  pages.forEach((page) => {
    const panel = document.getElementById(page);
    panel.hidden = page !== selected || (page === "home" && window.launcherCoreState?.phase === "running");
    if (panel.hidden && panel.contains(document.activeElement)) document.querySelector(`[data-page="${selected}"]`).focus();
  });
  resetPlayGlow();
  navigation.forEach((item) => {
    if (item.dataset.page === selected) item.setAttribute("aria-current", "page");
    else item.removeAttribute("aria-current");
  });
  document.querySelector("main").scrollTop = 0;
  updateInputView();
  updateAccountView();
}
navigation.forEach((button) => button.addEventListener("click", () => {
  showPage(button.dataset.page);
  windowCommand("page:" + button.dataset.page);
}));

function windowCommand(command) {
  window.chrome?.webview?.postMessage(command);
}
const play = document.getElementById("play");
let launchPending = false, launchRequestSession = 0;
play.addEventListener("click", () => {
  if (play.disabled || launchPending || settingsBusy() || window.launcherCoreState?.canLaunch !== true) return;
  launchPending = true;
  launchRequestSession = window.launcherCoreState.sessionId;
  play.disabled = true;
  windowCommand("game:start");
});
let glowFrame = 0;
function resetPlayGlow() {
  cancelAnimationFrame(glowFrame); glowFrame = 0;
  delete play.dataset.glow;
}
play.addEventListener("pointermove", event => {
  if (play.disabled || glowFrame) return;
  const x = event.clientX, y = event.clientY;
  glowFrame = requestAnimationFrame(() => {
    glowFrame = 0;
    if (play.disabled) return;
    const rect = play.getBoundingClientRect();
    play.style.setProperty("--mouse-x", `${(x - rect.left) * play.offsetWidth / rect.width}px`);
    play.style.setProperty("--mouse-y", `${(y - rect.top) * play.offsetHeight / rect.height}px`);
    play.dataset.glow = "true";
  });
});
for (const event of ["pointerleave","pointercancel","blur"]) play.addEventListener(event, resetPlayGlow);
window.addEventListener("blur", resetPlayGlow);
new MutationObserver(() => { if (play.disabled) resetPlayGlow(); }).observe(play, {attributes:true,attributeFilter:["disabled"]});
let returnAfterSaves = false;
document.querySelectorAll("[data-return-game]").forEach(button => button.addEventListener("click", () => {
  if (window.launcherCoreState?.canStopGame !== true) return;
  returnAfterSaves = true;
  drainSaves();
}));
const stopDialog = document.getElementById("stop-confirmation");
const stopCancel = document.getElementById("stop-cancel");
const stopConfirm = document.getElementById("stop-confirm");
let stopConfirmation = null;
let stopConfirmPending = false;
function validId(value) {
  return Number.isSafeInteger(value) && value > 0;
}
function cancelStop() {
  if (!stopConfirmation || stopConfirmPending) return;
  const { sessionId, confirmationId } = stopConfirmation;
  stopConfirmation = null;
  stopDialog.close();
  windowCommand(`game:stop-cancel:${sessionId}:${confirmationId}`);
}
stopCancel.addEventListener("click", cancelStop);
stopDialog.addEventListener("cancel", (event) => {
  event.preventDefault();
  cancelStop();
});
stopConfirm.addEventListener("click", () => {
  const state = window.launcherCoreState;
  if (!stopConfirmation || stopConfirmPending || state?.canStopGame !== true ||
      state.sessionId !== stopConfirmation.sessionId || state.stopConfirmationId !== stopConfirmation.confirmationId) return;
  stopConfirmPending = true;
  stopConfirm.disabled = stopCancel.disabled = true;
  windowCommand(`game:stop-confirm:${stopConfirmation.sessionId}:${stopConfirmation.confirmationId}`);
});
const coreLabels = {
  idle: ["準備中"], validating: ["準備中"], initializing: ["初期化中"], ready: ["準備完了"],
  preparing: ["起動中"], opengl: ["描画準備中"], vulkan: ["描画準備中"], running: ["プレイ中"],
  "title-stopping": ["終了中"], "title-stopped": ["終了"], stopping: ["終了中"], stopped: ["終了"],
  failed: ["起動失敗"], "relaunch-failed": ["再起動失敗"], unavailable: ["起動機能なし"],
};
window.addEventListener("launcher-core-state", (event) => {
  const state = event.detail;
  window.launcherCoreState = state;
  if (state.phase !== "ready" || state.sessionId !== launchRequestSession) launchPending = false;
  if (state.phase === "running") document.getElementById("home").hidden = true;
  const phase = state.failed ? (state.sessionId > 1 ? "relaunch-failed" : "failed") : state.stopRequested && !state.stopped ? "stopping" : state.phase;
  const [status] = coreLabels[phase] || coreLabels.idle;
  play.disabled = state.canLaunch !== true || launchPending || settingsBusy() || closeAfterSaves;
  const hasConfirmation = state.canStopGame === true && validId(state.stopConfirmationId) &&
    state.stopConfirmationSessionId === state.sessionId;
  if (hasConfirmation) {
    if (!stopConfirmation || stopConfirmation.confirmationId !== state.stopConfirmationId || stopConfirmation.sessionId !== state.sessionId) {
      stopConfirmation = { sessionId: state.sessionId, confirmationId: state.stopConfirmationId };
      stopConfirmPending = false;
      stopConfirm.disabled = stopCancel.disabled = false;
      // 実行中の確認表示は NativeGameHost が所有する。
    }
  } else {
    const wasOpen = stopDialog.open;
    stopConfirmation = null;
    stopConfirmPending = false;
    if (wasOpen) stopDialog.close();
  }
  document.querySelectorAll("[data-return-game]").forEach(button => { button.hidden = state.canStopGame !== true || hasConfirmation; button.disabled = settingsBusy(); });
  const closing = state.stopRequested === true || phase === "stopping" || phase === "stopped";
  const titleStopping = phase === "title-stopping" || phase === "title-stopped";
  navigation.forEach((button) => { button.disabled = closing || titleStopping || hasConfirmation; });
  document.getElementById("update-preferences").disabled = !confirmed.preferences || closing || titleStopping || hasConfirmation || closeAfterSaves;
  document.querySelectorAll("[data-tab]").forEach((tab) => { tab.disabled = closing || titleStopping || hasConfirmation || closeAfterSaves; });
  document.getElementById("core-information").textContent = status;
  updateNativeSettings(state);
  renderAccount();
  updateAccountView();
});

const tabs = Array.from(document.querySelectorAll("[data-tab]"));
let selectedTab = "general";
function showTab(name) {
  if (!tabs.some((tab) => tab.dataset.tab === name)) return;
  if (selectedTab !== name) stopInputView();
  if (selectedTab !== name) hideAccountDetails();
  selectedTab = name;
  tabs.forEach((tab) => {
    const selected = tab.dataset.tab === name;
    tab.setAttribute("aria-selected", String(selected));
    tab.tabIndex = selected ? 0 : -1;
    const panel = document.getElementById(tab.getAttribute("aria-controls"));
    if (!selected && panel.contains(document.activeElement)) tab.focus();
    panel.hidden = !selected;
  });
  document.querySelector(".settings-content").scrollTop = 0;
  updateInputView();
  updateAccountView();
}
tabs.forEach((tab, index) => {
  tab.addEventListener("click", () => showTab(tab.dataset.tab));
  tab.addEventListener("keydown", (event) => {
    let next;
    if (event.key === "ArrowRight") next = (index + 1) % tabs.length;
    if (event.key === "ArrowLeft") next = (index + tabs.length - 1) % tabs.length;
    if (event.key === "Home") next = 0;
    if (event.key === "End") next = tabs.length - 1;
    if (next === undefined) return;
    event.preventDefault();
    showTab(tabs[next].dataset.tab);
    tabs[next].focus();
  });
});

const nativeGraphics = document.getElementById("native-graphics");
const nativeFps = document.getElementById("native-fps");
const fpsSlider = document.getElementById("native-fps-slider");
const fpsOriginal = document.getElementById("native-fps-original");
const fpsError = document.getElementById("fps-error");
const nativeResolution = document.getElementById("native-resolution");
const preferenceInputs = ["automatic-updates", "check-on-startup", "preview-updates"].map((id) => document.getElementById(id));
const confirmed = { native: null, preferences: null };
const desired = { native: null, preferences: null };
const saveQueue = [];
let pendingSave = null;
let saveSequence = 0;
let closeAfterSaves = false;
let lastFps = 1000;
let editingFps = false;
let requestedSettingsInfo = false;
function settingsBusy() { return pendingSave !== null || saveQueue.length > 0 || pendingInput !== null || pendingAv !== null || avQueue.length > 0 || pendingAccount !== null || accountQueue.length > 0; }
function same(a, b) { return JSON.stringify(a) === JSON.stringify(b); }
function draft(scope) { return desired[scope] || confirmed[scope]; }
function updatePlay() {
  play.disabled = window.launcherCoreState?.canLaunch !== true || launchPending || settingsBusy() || closeAfterSaves;
}
function note(scope, text) {
  document.getElementById(scope === "native" ? "native-settings-note" : "preferences-note").textContent = text;
}
function enqueueSave(scope, values) {
  if (!confirmed[scope]) return;
  desired[scope] = values;
  if (!saveQueue.includes(scope)) saveQueue.push(scope);
  drainSaves();
}
function drainSaves() {
  if (pendingSave || pendingInput || pendingAv || pendingAccount) return;
  while (saveQueue.length) {
    const scope = saveQueue.shift();
    const values = desired[scope];
    if (same(values, confirmed[scope])) { desired[scope] = null; continue; }
    pendingSave = { scope, values, requestId: ++saveSequence };
    note(scope, "保存しています。保存完了まで Play は利用できません。");
    updatePlay();
    windowCommand((scope === "native" ? "settings:save:" : "preferences:save:") + pendingSave.requestId + ":" + JSON.stringify(values));
    return;
  }
  updatePlay();
  if (avQueue.length) { drainAv(); return; }
  if (accountQueue.length) { drainAccount(); return; }
  if (closeAfterSaves) windowCommand("window:close-confirmed");
  else if (returnAfterSaves) { returnAfterSaves = false; stopInputView(); windowCommand("game:return"); }
}
function renderNative(force = false) {
  const values = draft("native");
  if (!values) return;
  const original = values.native_vsync_frequency === 0;
  fpsOriginal.checked = original;
  fpsSlider.disabled = nativeFps.disabled = original;
  if (force || !editingFps) {
    nativeFps.value = fpsSlider.value = String(original ? lastFps : values.native_vsync_frequency);
  }
  nativeResolution.value = `${values.native_internal_width}x${values.native_internal_height}`;
}
function renderPreferences() {
  const values = draft("preferences");
  if (!values) return;
  preferenceInputs.forEach((input, index) => { input.checked = values[["automatic_updates", "check_on_startup", "preview_updates"][index]]; });
}
function saveFps(value) {
  if (nativeGraphics.disabled || !confirmed.native) return;
  fpsError.textContent = "";
  editingFps = false;
  const values = { ...draft("native"), native_vsync_frequency: value };
  enqueueSave("native", values);
  renderNative(true);
}
function validFps(value) { return /^[0-9]{2,4}$/.test(value) && Number(value) >= 30 && Number(value) <= 1000; }
function commitFps() {
  if (!editingFps) return;
  if (validFps(nativeFps.value)) saveFps(Number(nativeFps.value));
  else {
    editingFps = false;
    fpsError.textContent = "30〜1000の整数を入力してください。最後に保存された有効値へ戻しました。";
    const saved = confirmed.native?.native_vsync_frequency || lastFps;
    nativeFps.value = fpsSlider.value = String(saved);
  }
}
nativeFps.addEventListener("input", () => {
  editingFps = true;
  if (validFps(nativeFps.value)) fpsSlider.value = nativeFps.value;
});
nativeFps.addEventListener("paste", (event) => {
  const text = event.clipboardData?.getData("text") || "";
  if (!/^[0-9]{1,4}$/.test(text)) {
    event.preventDefault();
    fpsError.textContent = "4桁以内の整数を入力してください。";
  }
});
nativeFps.addEventListener("keydown", (event) => {
  if (event.key === "Enter") { event.preventDefault(); commitFps(); }
});
nativeFps.addEventListener("blur", commitFps);
fpsSlider.addEventListener("input", () => { editingFps = true; nativeFps.value = fpsSlider.value; fpsError.textContent = ""; });
fpsSlider.addEventListener("change", () => saveFps(Number(fpsSlider.value)));
fpsOriginal.addEventListener("change", () => saveFps(fpsOriginal.checked ? 0 : lastFps));
nativeResolution.addEventListener("change", () => {
  if (nativeGraphics.disabled || !confirmed.native) return;
  const [width, height] = nativeResolution.value.split("x").map(Number);
  enqueueSave("native", { ...draft("native"), native_internal_width: width, native_internal_height: height });
});
preferenceInputs.forEach((input) => input.addEventListener("change", () => {
  enqueueSave("preferences", { config_version: 1, automatic_updates: preferenceInputs[0].checked, check_on_startup: preferenceInputs[1].checked, preview_updates: preferenceInputs[2].checked });
}));
window.addEventListener("launcher-preferences", (event) => {
  confirmed.preferences = event.detail.values;
  document.getElementById("launcher-version").textContent = event.detail.version;
  document.getElementById("update-preferences").disabled = false;
  renderPreferences();
  note("preferences", event.detail.notice === "load-failed" ? "設定を読み込めなかったため、更新設定はすべて OFF を使用します。" : "設定は変更時に自動保存されます。更新処理はまだ行いません。");
});
window.addEventListener("launcher-settings-result", (event) => {
  const result = event.detail;
  if (!pendingSave || result.requestId !== pendingSave.requestId || result.scope !== pendingSave.scope) return;
  const scope = result.scope;
  const values = scope === "native" ? result.state.nativeSettings : result.values;
  const notice = scope === "native" ? result.state.settingsNotice : result.notice;
  confirmed[scope] = values;
  if (scope === "native" && values.native_vsync_frequency) lastFps = values.native_vsync_frequency;
  const newer = saveQueue.includes(scope);
  if (!newer) desired[scope] = null;
  pendingSave = null;
  note(scope, notice === "saved" ? (scope === "native" ? "設定を保存しました。次回Playで適用されます。" : "更新設定を保存しました。更新処理はまだ行いません。") : "設定を保存できませんでした。保存済みの値へ戻しました。もう一度操作すると再試行できます。");
  if (scope === "native") renderNative(); else renderPreferences();
  drainSaves();
});
window.addEventListener("launcher-close-request", () => {
  commitFps();
  stopInputView();
  closeAfterSaves = true;
  nativeGraphics.disabled = document.getElementById("update-preferences").disabled = true;
  navigation.forEach((button) => { button.disabled = true; });
  tabs.forEach((tab) => { tab.disabled = true; });
  play.disabled = true;
  drainSaves();
});
function setInfo(id, entries) {
  const list = document.getElementById(id);
  const key = JSON.stringify(entries);
  if (list.dataset.snapshot === key) return;
  list.dataset.snapshot = key;
  list.replaceChildren();
  entries.forEach(([label, value]) => {
    const term = document.createElement("dt"), detail = document.createElement("dd");
    term.textContent = label;
    detail.textContent = String(value);
    list.append(term, detail);
  });
}
function updateNativeSettings(state) {
  nativeGraphics.disabled = state.canEditNativeSettings !== true || closeAfterSaves;
  if (!state.nativeSettings) {
    note("native", state.phase === "unavailable" ? "Core がないためゲーム設定は利用できません。" : "ゲームの起動準備を待っています。");
    setInfo("audio-info", [["現在の設定", "Core の情報は利用できません。"]]);
    setInfo("controller-info", [["接続状態", "Core の情報は利用できません。"]]);
    return;
  }
  confirmed.native = state.nativeSettings;
  if (state.nativeSettings.native_vsync_frequency) lastFps = state.nativeSettings.native_vsync_frequency;
  if (state.canEditNativeSettings !== true) editingFps = false;
  renderNative();
  if (!pendingSave && !saveQueue.includes("native")) {
    const notices = { default: "FPS 1000・内部1920×1080を初期値として使用します。変更時に自動保存します。", loaded: "保存済みの設定です。変更は次回Playで適用されます。", saved: "設定を保存しました。次回Playで適用されます。", "load-failed": "設定を読み込めなかったため、FPS 1000・内部1920×1080を使用します。", "save-failed": "設定を保存できませんでした。保存済みの値を表示しています。", invalid: "設定値が無効なため、保存しませんでした。", "not-ready": "ゲーム停止後、準備完了になると変更できます。" };
    note("native", (notices[state.settingsNotice] || "ゲームの起動準備を待っています。") + (state.canEditNativeSettings === true ? "" : " ゲームの実行中・起動中・停止中は変更できません。"));
  }
  renderInput(state.settingsInfo?.input, state);
  renderAv(state.settingsInfo?.av, state);
  const info = state.settingsInfo;
  if (!info?.audioBackend && state.corePhase === "ready" && ["ready", "running"].includes(state.phase) && !requestedSettingsInfo) {
    requestedSettingsInfo = true;
    windowCommand("settings:get");
  }
  if (info?.audioBackend) {
    setInfo("audio-info", [["Audio Backend", info.audioBackend], ["TV Output Device", info.tvDevice], ["TV Volume", info.tvVolume], ["GamePad Output Device", info.padDevice], ["GamePad Volume", info.padVolume]]);
    const connected = (info.controllers || []).filter((device) => device.connected && device.api === "SDLController");
    if (!info.input?.mapping) setInfo("controller-info", [["接続状態", connected.length ? "接続中" : "未接続"], ["接続中のコントローラー", connected.map((device) => device.name).join(" / ") || "なし"], ["VPAD割当", info.vpadAssigned ? "割当済み" : "未割当"], ["Controller Profile", info.profile]]);
  }
}

window.addEventListener("launcher-session-ended", () => showPage("home"));
window.addEventListener("launcher-show-home", () => showPage("home"));
window.addEventListener("launcher-return-game", () => showPage("home"));
window.addEventListener("launcher-display-state", (event) => {
  window.launcherDisplayState = event.detail;
});
document.getElementById("window-minimize").addEventListener("click", () => windowCommand("window:minimize"));
document.getElementById("window-maximize").addEventListener("click", () => windowCommand("window:toggle-maximize"));
document.getElementById("window-close").addEventListener("click", () => windowCommand("window:close"));
window.addEventListener("launcher-window-state", (event) => {
  const maximized = event.detail === true;
  document.documentElement.dataset.maximized = String(maximized);
  const button = document.getElementById("window-maximize");
  const label = maximized ? "元に戻す" : "最大化";
  button.setAttribute("aria-label", label);
  button.title = label;
});

const dragRegion = document.getElementById("titlebar-drag");
let dragStart = null;
dragRegion.addEventListener("pointerdown", (event) => {
  if (event.button === 0) dragStart = { x: event.clientX, y: event.clientY };
});
dragRegion.addEventListener("pointermove", (event) => {
  if (!dragStart || !(event.buttons & 1)) { dragStart = null; return; }
  if (Math.hypot(event.clientX - dragStart.x, event.clientY - dragStart.y) < 4) return;
  dragStart = null;
  windowCommand("window:start-drag");
});
window.addEventListener("pointerup", () => { dragStart = null; });
dragRegion.addEventListener("pointercancel", () => { dragStart = null; });
dragRegion.addEventListener("dblclick", (event) => {
  if (event.button === 0) windowCommand("window:toggle-maximize");
});

const inputControls = document.getElementById("input-controls");
const inputEdit = document.getElementById("input-edit");
const inputNote = document.getElementById("input-note");
const physicalController = document.getElementById("physical-controller");
const profileSelect = document.getElementById("controller-profile");
const mappingGrid = document.getElementById("controller-mapping");
const captureCancel = document.getElementById("input-capture-cancel");
const resetConfirmation = document.getElementById("input-reset-confirmation");
let inputSnapshot = null, pendingInput = null;
let inputSequence = Date.now() * 1000, inputViewId = 1;
let settingsTimer = null, inputTimer = null, inputPollPending = false, inputPollDeadline = 0, lastCaptureId = 0;
const inputMessages = {
  ready:"変更は自動保存されます。ゲームに戻ると新しい割当を使用します。",
  saved:"Profile を保存し、再読込を確認しました。",
  "saved-duplicate":"保存しました。同じ物理入力が複数の割当に設定されています。",
  "save-failed":"保存できませんでした。保存前の設定へ戻しました。もう一度操作すると再試行できます。",
  "recovery-required":"Profile の復旧が必要です。バックアップを保護してランチャーを終了してください。",
  "stale-request":"古い操作は反映しませんでした。画面の最新の設定を確認してください。",
  rejected:"操作を反映できませんでした。Profile 名・重複する名前・機器の接続状態を確認してください。",
  "not-ready":"起動・終了処理中は変更できません。",
  "release-inputs":"すべてのボタンとスティックを離してください（10秒以内）。",
  "press-input":"ボタンを押してください。スティック・トリガーは大きく動かしてください。",
  "capture-timeout":"入力待機を終了しました。もう一度割当を選択してください。",
  "capture-cancelled":"入力待機をキャンセルしました。",
  disconnected:"Disconnected · 接続を確認し、一覧から選択してください。",
  reconnected:"コントローラーへ自動接続しました。保存済みの割当を復元しました。",
  "connected-new-model":"新しい機器へ接続しました。機器別の割当を使用し、元の Profile は保持しました。",
  "duplicate-input":"この入力は連続割当で使用済みです。入力を離し、別のボタンを押してください。",
  "select-controller":"接続中のコントローラーを一覧から選択してください。",
  "unsupported-profile":"現在の Profile は編集対象外です。物理コントローラーを選択すると SDLController の割当を作成します。",
};
function inputActive() { return !document.getElementById("settings").hidden && selectedTab === "input" && !document.hidden && !closeAfterSaves; }
function inputCommand(values) {
  if (pendingInput || pendingSave || pendingAv || closeAfterSaves || window.launcherCoreState?.canEditInput !== true) return;
  pendingInput = ++inputSequence;
  inputControls.disabled = true;
  updatePlay();
  windowCommand("input:command:" + pendingInput + ":" + JSON.stringify({ ...values, revision:inputSnapshot?.revision }));
}
function options(select, items, selectedValue) {
  const key = JSON.stringify(items);
  if (select.dataset.snapshot !== key) {
    select.dataset.snapshot = key;
    select.replaceChildren(...items.map(([value, label]) => new Option(label, value)));
  }
  select.value = selectedValue;
}
function renderInput(info, state = window.launcherCoreState) {
  if (info?.mapping) inputSnapshot = info;
  const current = inputSnapshot;
  const editable = state?.canEditInput === true && !pendingInput && !closeAfterSaves;
  inputControls.disabled = !editable;
  inputEdit.disabled = !current?.connected;
  document.getElementById("deadzone-controls").disabled = !editable || !current?.connected;
  if (!current) return;
  if (!pendingInput) inputNote.textContent = editable ? (inputMessages[current.notice] || inputMessages.ready) : inputMessages["not-ready"];
  options(physicalController, [["", "コントローラーを選択"], ...current.devices.map((d) => [String(d.token), d.name + (current.devices.filter(other => other.name === d.name).length > 1 ? ` (${current.devices.filter(other => other.name === d.name).indexOf(d) + 1})` : "")])], String(current.devices.find((d) => d.selected)?.token || ""));
  const chosenProfile = profileSelect.value;
  options(profileSelect, [["", "Profile を選択"], ...current.profiles.map((name) => [name, name])], current.profiles.includes(chosenProfile) ? chosenProfile : current.profiles.includes(current.profile) ? current.profile : "");
  setInfo("controller-info", [["Controller Status", current.connected ? "Connected" : "Disconnected"], ["Controller Profile", current.profile], ["Player", "1 · Wii U GamePad"]]);
  for (const [side, value] of [["left", current.leftDeadzone], ["right", current.rightDeadzone]]) {
    const slider = document.getElementById(side + "-deadzone");
    if (document.activeElement !== slider || !editable || ["save-failed","rejected"].includes(current.notice)) {
      slider.value = String(value);
      document.getElementById(side + "-deadzone-value").value = Number(value).toFixed(2);
    }
  }
  updateDeadzonePreview();
  const key = JSON.stringify(current.mapping);
  if (mappingGrid.dataset.snapshot !== key) {
    mappingGrid.dataset.snapshot = key;
    mappingGrid.replaceChildren(...current.mapping.map((m) => {
      const row = document.createElement("div"), label = document.createElement("span"), button = document.createElement("button");
      row.className = "mapping-row"; row.dataset.mapping = m.id; row.dataset.button = m.button ?? "";
      label.textContent = m.label; button.textContent = (m.name || "未割当") + (m.duplicate ? "（重複）" : "");
      button.type = "button"; button.setAttribute("aria-label", m.label + "の割当 " + (m.name || "未割当"));
      button.addEventListener("click", () => inputCommand({ op:"capture", mapping:m.id, continuous:document.getElementById("mapping-continuous").checked }));
      row.append(label, button); return row;
    }));
  }
}
physicalController.addEventListener("change", () => { if (physicalController.value) inputCommand({ op:"select", token:Number(physicalController.value) }); });
document.getElementById("profile-load").addEventListener("click", () => { if (profileSelect.value) inputCommand({ op:"load", name:profileSelect.value }); });
document.getElementById("profile-save-as").addEventListener("click", () => inputCommand({ op:"save-as", name:document.getElementById("profile-name").value }));
for (const side of ["left", "right"]) {
  const slider = document.getElementById(side + "-deadzone");
  slider.addEventListener("input", () => { document.getElementById(side + "-deadzone-value").value = Number(slider.value).toFixed(2); updateDeadzonePreview(); });
  slider.addEventListener("change", () => inputCommand({ op:"deadzone", left:Number(document.getElementById("left-deadzone").value), right:Number(document.getElementById("right-deadzone").value) }));
}
document.getElementById("input-reset").addEventListener("click", () => { resetConfirmation.hidden = false; document.getElementById("input-reset-cancel").focus(); });
document.getElementById("input-reset-cancel").addEventListener("click", () => { resetConfirmation.hidden = true; document.getElementById("input-reset").focus(); });
document.getElementById("input-reset-confirm").addEventListener("click", () => { resetConfirmation.hidden = true; inputCommand({ op:"reset" }); });
captureCancel.addEventListener("click", () => { windowCommand("input:cancel"); lastCaptureId = 0; document.getElementById("capture-actions").hidden = true; inputNote.textContent = inputMessages["capture-cancelled"]; });
document.getElementById("input-capture-skip").addEventListener("click", () => inputCommand({op:"skip"}));
document.getElementById("input-capture-finish").addEventListener("click", () => inputCommand({op:"finish"}));
function updateDeadzonePreview() {
  for (const side of ["left","right"]) {
    const value = Number(document.getElementById(side + "-deadzone").value);
    document.querySelector("#" + side + "-stick-test .deadzone-circle").style.width = value * 100 + "%";
    document.querySelector("#" + side + "-stick-test .deadzone-circle").style.height = value * 100 + "%";
  }
}
window.addEventListener("launcher-input-result", (event) => {
  if (event.detail.requestId !== pendingInput) return;
  pendingInput = null;
  renderInput(event.detail.input);
  if (event.detail.input.notice) inputNote.textContent = inputMessages[event.detail.input.notice] || inputMessages.ready;
  drainSaves(); updatePlay();
});
function clearLiveInput() {
  for (const side of ["left", "right"]) {
    document.querySelectorAll("#" + side + "-stick-test span").forEach(point => point.style.transform = "translate(-50%,-50%)");
    document.getElementById(side + "-stick-value").value = "Raw 0.00 / 0.00 · Output 0.00 / 0.00";
  }
  mappingGrid.querySelectorAll(".mapping-row").forEach((row) => { row.dataset.pressed = "false"; });
  document.getElementById("trigger-test").textContent = "ZL 0.00 / ZR 0.00";
  document.getElementById("button-test").textContent = "未接続";
}
window.addEventListener("launcher-input-live", (event) => {
  if (!inputActive() || event.detail.viewId !== inputViewId) return;
  inputPollPending = false;
  const state = event.detail.state;
  if (!state.connected) clearLiveInput();
  else {
    for (const side of ["left", "right"]) {
      const [x,y] = state[side], raw = state[side === "left" ? "rawLeft" : "rawRight"];
      const test = document.getElementById(side + "-stick-test"), radius = test.clientWidth / 2 - 5;
      for (const [selector, values] of [[".raw-point",raw],[".processed-point",[x,y]]])
        test.querySelector(selector).style.transform = `translate(calc(-50% + ${values[0] * radius}px),calc(-50% + ${values[1] * radius}px))`;
      document.getElementById(side + "-stick-value").value = `Raw ${raw[0].toFixed(2)} / ${raw[1].toFixed(2)} · Output ${x.toFixed(2)} / ${y.toFixed(2)}`;
    }
    document.getElementById("trigger-test").textContent = `ZL ${state.triggers[0].toFixed(2)} / ZR ${state.triggers[1].toFixed(2)}`;
    document.getElementById("button-test").textContent = state.buttons.map((id) => inputSnapshot?.buttons?.find((b) => b.id === id)?.name || `Button ${id}`).join(" · ") || "ボタン解放";
  }
  mappingGrid.querySelectorAll(".mapping-row").forEach((row) => { row.dataset.pressed = String(state.connected === true && row.dataset.button !== "" && state.buttons.includes(Number(row.dataset.button))); });
  document.getElementById("capture-actions").hidden = !state.captureId;
  document.getElementById("input-capture-skip").disabled = !state.continuous || pendingInput !== null;
  if (inputSnapshot && state.revision) inputSnapshot.revision = state.revision;
  mappingGrid.querySelectorAll(".mapping-row").forEach(row => {
    const id = Number(row.dataset.mapping);
    row.dataset.capturing = String(!!state.captureId && id === state.captureMapping);
    row.dataset.complete = String(state.completed?.includes(id) === true);
    row.dataset.next = String(!!state.captureId && state.continuous && id === state.captureMapping + 1);
  });
  if (state.captureId || lastCaptureId) inputNote.textContent = (state.captureId ? (inputSnapshot?.mapping.find(m => m.id === state.captureMapping)?.label || "") + " · " : "") + (inputMessages[state.notice] || inputMessages.ready);
  if (lastCaptureId && !state.captureId) windowCommand("settings:get");
  lastCaptureId = state.captureId || 0;
});
function stopInputView() {
  if (inputTimer !== null) clearInterval(inputTimer);
  if (settingsTimer !== null) clearInterval(settingsTimer);
  inputTimer = settingsTimer = null;
  inputPollPending = false; inputViewId++; lastCaptureId = 0;
  windowCommand("input:cancel");
  document.getElementById("capture-actions").hidden = true; resetConfirmation.hidden = true;
  clearLiveInput();
}
function updateInputView() {
  if (document.getElementById("settings").hidden || document.hidden || closeAfterSaves) return;
  if (settingsTimer === null) {
    windowCommand("settings:get");
    settingsTimer = setInterval(() => windowCommand("settings:get"), 1000);
  }
  if (selectedTab === "input" && inputTimer === null) inputTimer = setInterval(() => {
    if (!inputActive() || (inputPollPending && performance.now() < inputPollDeadline) || !document.hasFocus() || !["ready","running"].includes(window.launcherCoreState?.phase)) return;
    inputPollPending = true; inputPollDeadline = performance.now() + 500;
    windowCommand("input:poll:" + inputViewId);
  }, 40);
}
document.addEventListener("visibilitychange", () => { stopInputView(); updateInputView(); });
window.addEventListener("blur", () => { windowCommand("input:cancel"); clearLiveInput(); });

const avKeys = ["graphicsApi","gpu","vsync","async","fullSync","upscale","downscale","scaling","audioApi","latency","tvDevice","tvChannels","tvVolume","padDevice","padVolume","micDevice","micVolume"];
const graphicsAvKeys = avKeys.slice(0, 8);
const overlayAvKeys = ["overlayPosition","overlayColor","overlayScale","overlayFps","overlayDrawCalls","overlayCpu","overlayCpuPerCore","overlayRam","overlayVram","overlayDebug","notificationPosition","notificationColor","notificationScale","notificationProfiles","notificationBattery","notificationShaders","notificationFriends"];
avKeys.push(...overlayAvKeys);
function avCategory(key) { return graphicsAvKeys.includes(key) ? "graphics" : overlayAvKeys.includes(key) ? "overlay" : "audio"; }
function canEditAv(key, state = window.launcherCoreState) { return state?.[avCategory(key) === "graphics" ? "canEditNativeSettings" : avCategory(key) === "overlay" ? "canEditOverlay" : "canEditAudio"] === true; }
function colorHex(rgba) { return "#" + [rgba & 255, (rgba >>> 8) & 255, (rgba >>> 16) & 255].map(channel => channel.toString(16).padStart(2,"0")).join("").toUpperCase(); }
function colorRgba(hex, alpha) { return (parseInt(hex.slice(1,3),16) | parseInt(hex.slice(3,5),16) << 8 | parseInt(hex.slice(5,7),16) << 16 | alpha << 24) >>> 0; }

const avQueue = [];
let avSnapshot = null, pendingAv = null, avSequence = Date.now() * 1000;
const avLabels = ["DirectSound","XAudio2 2.7","XAudio2","Cubeb"];
function avNote(key,text) { document.getElementById(avCategory(key) + "-settings-note").textContent = text; }
function renderAv(info, state = window.launcherCoreState) {
  if (info?.values) avSnapshot = info;
  const current = avSnapshot;
  for (const [id,key] of [["av-graphics","graphicsApi"],["av-audio","audioApi"],["av-overlay","overlayPosition"]]) document.getElementById(id).disabled = !canEditAv(key,state) || closeAfterSaves || pendingAv !== null || !!state?.stopConfirmationId;
  if (!current) return;
  const values = current.values;
  const select = (key, items) => {
    const value = String(values[key]);
    if (!items.some(item => String(item[0]) === value)) items.push([value,"保存したデバイス（利用できません）"]);
    options(document.getElementById("av-" + key),items,value);
    for (const option of document.getElementById("av-" + key).options) option.disabled = Number(option.value) < 0;
  };
  if (values.graphicsApi === 0) options(document.getElementById("av-gpu"),[[0,"System Default"]],"0");
  else select("gpu", [[0,"Default"], ...current.gpus.map(d => [d.token,d.name])]);
  select("vsync", values.graphicsApi === 0 ? [[0,"Off"],[1,"On"]] : [[0,"Off"],[1,"Double Buffering"],[2,"Triple Buffering"],[3,"Match Emulated Display (Experimental)"]]);
  select("audioApi",current.audioApis.map(api => [api,avLabels[api]]));
  for (const key of ["tvDevice","padDevice","micDevice"])
    select(key,[[0,"Disabled"], ...(key === "micDevice" ? current.inputs : current.outputs).map(d => [d.token,d.name])]);
  select("tvChannels",current.tvChannels.map(channel => [channel,["Mono","Stereo","Surround"][channel]]));
  for (const key of avKeys) {
    if (key === "gpu" && values.graphicsApi === 0) continue;
    if (pendingAv?.key === key || avQueue.some(item => item.key === key)) continue;
    const control = document.getElementById("av-" + key);
    if (control.type === "color") {
      control.value = colorHex(values[key]);
      const hex = document.getElementById(control.id + "-hex"), alpha = document.getElementById(control.id + "-alpha");
      if (document.activeElement !== hex) { hex.value = control.value.toUpperCase(); hex.setCustomValidity(""); }
      if (document.activeElement !== alpha) alpha.value = String(values[key] >>> 24);
    }
    else if (control.type === "checkbox") control.checked = values[key];
    else if (control.type !== "range" || document.activeElement !== control || ["save-failed","rejected"].includes(current.notice)) control.value = String(values[key]);
    if (control.type === "range") document.getElementById("av-" + key + "-value").value = Number(control.value) * (key === "latency" ? 12 : 1) + (key === "latency" ? " ms" : "%");
  }
  document.getElementById("av-gpu").disabled = document.getElementById("av-async").disabled = values.graphicsApi === 0;
  document.getElementById("renderer-info").textContent = state?.rendererDevice ? `現在の描画: ${state.activeRenderer} · ${state.rendererDevice}` : "";
  const missing = ["gpu","tvDevice","padDevice","micDevice"].filter(key => values[key] === -1);
  if (!pendingAv && current.notice !== "save-failed" && current.notice !== "rejected") {
    document.getElementById("graphics-settings-note").textContent = missing.includes("gpu") ? "保存したGPUが見つかりません。設定を保持し、起動時は既定GPUを使用します。" : "自動保存 · Applies on Next Play";
    document.getElementById("audio-settings-note").textContent = missing.some(key => key !== "gpu") ? "保存した音声デバイスが見つかりません。設定を保持しています。接続または選択を確認してください。" : current.enumerationNotice ? "デバイス一覧を取得できませんでした。再接続を確認してください。" : state?.phase === "running" ? "自動保存 · 実行中の音声へ反映します。" : "自動保存";
    document.getElementById("overlay-settings-note").textContent = "自動保存 · ゲーム実行中にも反映されます。";
  }
  document.getElementById("audio-stream-note").textContent = state?.phase === "running" ? `TV Stream: ${current.tvStream ? "作成済み" : values.tvDevice === 0 ? "Disabled" : "未初期化・出力を確認してください"} · GamePad Stream: ${current.padStream ? "作成済み" : values.padDevice === 0 ? "Disabled" : "未初期化"}` : "";
}
function avCommand(key,value) {
  if (!avSnapshot || closeAfterSaves || !canEditAv(key)) return;
  const existing = avQueue.find(item => item.key === key);
  if (existing) existing.value = value; else avQueue.push({key,value});
  updatePlay(); drainSaves();
}
function drainAv() {
  if (pendingAv || pendingSave || pendingInput || pendingAccount || !avQueue.length) return;
  pendingAv = {...avQueue.shift(),requestId:++avSequence};
  avNote(pendingAv.key,"適用・保存しています…");
  windowCommand("av:command:" + pendingAv.requestId + ":" + JSON.stringify({key:pendingAv.key,value:pendingAv.value,revision:avSnapshot.revision}));
  renderAv(); updatePlay();
}
for (const key of avKeys) {
  const control = document.getElementById("av-" + key);
  if (control.type === "color") {
    const hex = document.getElementById(control.id + "-hex"), alpha = document.getElementById(control.id + "-alpha");
    const commit = value => {
      const opacity = Number(alpha.value);
      if (alpha.value === "" || !/^#[0-9A-Fa-f]{6}$/.test(value) || !Number.isInteger(opacity) || opacity < 0 || opacity > 255) { avNote(key,"#RRGGBB と Alpha 0〜255 を入力してください。無効な値は保存しません。"); return; }
      hex.setCustomValidity(""); control.value = hex.value = value.toUpperCase(); avCommand(key,colorRgba(value,opacity));
    };
    control.addEventListener("change", () => commit(control.value));
    hex.addEventListener("change", () => commit(hex.value));
    hex.addEventListener("input", () => hex.setCustomValidity(/^#[0-9A-Fa-f]{6}$/.test(hex.value) ? "" : "#RRGGBB を入力してください。"));
    alpha.addEventListener("change", () => commit(control.value));
  } else control.addEventListener("change", () => avCommand(key,control.type === "checkbox" ? control.checked : Number(control.value)));
  if (control.type === "range") control.addEventListener("input", () => {
    document.getElementById("av-" + key + "-value").value = Number(control.value) * (key === "latency" ? 12 : 1) + (key === "latency" ? " ms" : "%");
  });
}
window.addEventListener("launcher-av-result", event => {
  if (!pendingAv || pendingAv.requestId !== event.detail.requestId) return;
  const key = pendingAv.key, result = event.detail.settings;
  if (result.values) avSnapshot = result;
  pendingAv = null;
  renderAv();
  avNote(key,result.notice === "saved" ? (avCategory(key) === "graphics" ? "Saved · 次回Playで適用されます。" : "Applied · Saved") : result.notice === "recovery-required" ? "Failed · 設定の復旧が必要です。ランチャーを終了してください。" : "Failed · 保存済みの値へ戻しました。もう一度操作すると再試行できます。");
  drainSaves(); updatePlay();
});


const accountQueue = [];
let accountSnapshot = null, pendingAccount = null, accountSequence = Date.now() * 1000;
let accountDetailsVisible = false, accountEditRequested = false, accountEditorId = null, accountConfirmationAction = null;
const accountAddDialog = document.getElementById("account-add-dialog");
let accountFocusTarget = null, accountConfirmationOpener = null;
function importInProgress() { return pendingAccount?.command.op === "import" || accountQueue.some(c => c.op === "import"); }
function clearAccountPreview() {
  delete document.getElementById("account-drop").dataset.drag;
  document.getElementById("account-add-notice").textContent = "";
  document.getElementById("account-online-replace").checked = false;
  document.querySelector("#account-preview details").open = false;
  if (accountSnapshot) Object.assign(accountSnapshot, {candidates:[],onlineFiles:[],previewId:0,previewNotice:"",hasOtp:false,hasSeeprom:false,onlineComplete:false});
  document.getElementById("account-preview").hidden = true;
  document.getElementById("account-candidates").replaceChildren();
  document.getElementById("account-online-files").replaceChildren();
  delete document.getElementById("account-candidates").dataset.snapshot;
  delete document.getElementById("account-online-files").dataset.snapshot;
}
document.getElementById("account-add").addEventListener("click", () => {
  if (settingsBusy() || window.launcherCoreState?.canEditAccounts !== true || accountSnapshot?.canEdit !== true || accountSnapshot.accounts.length >= 12) return;
  accountFocusTarget = null;
  hideAccountDetails(); clearAccountPreview();
  accountAddDialog.showModal();
  document.getElementById("account-add-cancel").focus();
  accountCommand({op:"clear-preview"});
});
document.getElementById("account-add-cancel").addEventListener("click", () => { if (!importInProgress()) accountAddDialog.close(); });
accountAddDialog.addEventListener("cancel", event => { if (importInProgress()) event.preventDefault(); });
accountAddDialog.addEventListener("close", () => {
  for (let i = accountQueue.length - 1; i >= 0; --i) if (["select-zip","drop","import"].includes(accountQueue[i].op)) accountQueue.splice(i,1);
  accountConfirmationAction = null;
  if (accountConfirmationDialog.open) accountConfirmationDialog.close();
  clearAccountPreview(); accountCommand({op:"clear-preview"});
  accountFocusTarget = document.getElementById("account-add");
  restoreAccountFocus();
});
function restoreAccountFocus() {
  if (!accountFocusTarget || settingsBusy()) return;
  let target = accountFocusTarget;
  accountFocusTarget = null;
  if (selectedTab !== "account" || document.getElementById("settings").hidden || window.launcherCoreState?.canEditAccounts !== true) return;
  const modal = document.querySelector("dialog[open]");
  if (modal && !modal.contains(target)) return;
  if (target.matches(":disabled")) target = modal ? document.getElementById("account-add-cancel") : document.getElementById("account-active");
  target.focus({preventScroll:true});
}
const accountEditor = document.getElementById("account-editor");
const accountConfirmationDialog = document.getElementById("account-confirmation");
const accountNotices = {
  saved: "Saved · 自動保存しました。", ready: "", "preview-ready": "ZIPを検証しました。対象を確認してからインポートしてください。",
  "preview-failed": "ZIPを取り込めません。構造・サイズ・整合性を確認してください。", "not-ready": "アカウントの変更はMinecraft終了後に行えます。",
  "last-account": "最後の1アカウントは削除できません。", "account-limit": "アカウントは最大12件です。", "invalid-name": "Mii名は空白だけを避け、10文字以内で入力してください。",
  "invalid-birthday": "有効な生年月日を入力してください。", "invalid-email": "有効なメールアドレスを入力してください。", "invalid-value": "値の範囲を確認してください。",
  "stale-revision": "設定が更新されました。表示された状態を確認して再試行してください。", "stale-preview": "プレビューを再取得してください。",
  "persistent-id-exists": "同じPersistent IDが存在します。上書きせず中止しました。", "id-unavailable": "新しいアカウントIDを確保できませんでした。",
  "save-failed": "保存できませんでした。元のデータへ戻しました。", "recovery-required": "復旧が必要です。ランチャーを終了し、バックアップを確認してください。",
  "preferences-invalid": "Network Serviceの設定を読み込めません。Offlineを使用します。"
};
function hideAccountDetails() {
  accountDetailsVisible = accountEditRequested = false;
  if (accountSnapshot) delete accountSnapshot.details;
  const details = document.getElementById("account-details");
  details.hidden = true; details.replaceChildren(); delete details.dataset.snapshot;
  const button = document.getElementById("account-details-toggle");
  button.textContent = "Show Details"; button.setAttribute("aria-expanded", "false");
  if (accountEditor.open) accountEditor.close();
  if (accountAddDialog.open && !importInProgress()) accountAddDialog.close();
  if (accountConfirmationDialog.open) accountConfirmationDialog.close();
  document.getElementById("account-email").value = document.getElementById("account-birthday").value = "";
  accountEditorId = null;
}
function updateAccountView() {
  if (selectedTab !== "account" || document.getElementById("settings").hidden || pendingAccount || accountQueue.length) return;
  if ((!accountSnapshot || accountSnapshot.canEdit !== window.launcherCoreState?.canEditAccounts) && window.launcherCoreState?.corePhase === "ready") accountCommand({op:"list"});
  renderAccount();
}
function accountCommand(command) {
  if (closeAfterSaves || !window.launcherCoreState || (command.op !== "list" && command.op !== "details" && window.launcherCoreState.canEditAccounts !== true)) return;
  accountQueue.push(command); updatePlay(); drainSaves();
}
function drainAccount() {
  if (pendingAccount || pendingSave || pendingInput || pendingAv || !accountQueue.length) return;
  const command = accountQueue.shift();
  if (command.op !== "list" && command.op !== "details") command.revision = accountSnapshot?.revision;
  pendingAccount = {requestId:++accountSequence,command};
  renderAccount(); updatePlay();
  if (command.op === "select-custom") windowCommand("account:select-custom:" + pendingAccount.requestId + ":" + (command.replaceExisting ? "1" : "0"));
  else if (command.op === "select-zip") windowCommand("account:select-zip:" + pendingAccount.requestId);
  else if (command.op === "drop") {
    try {
      if (typeof window.chrome?.webview?.postMessageWithAdditionalObjects !== "function") throw new Error("unavailable");
      window.chrome.webview.postMessageWithAdditionalObjects("account:drop:" + pendingAccount.requestId, [command.file]);
    } catch {
      pendingAccount = null;
      document.getElementById("account-notice").textContent = "この環境ではドロップできません。Select ZIP Fileを使用してください。";
      renderAccount(); drainSaves();
    }
  } else windowCommand("account:command:" + pendingAccount.requestId + ":" + JSON.stringify(command));
}
function renderAccount() {
  const state = window.launcherCoreState;
  const canEdit = state?.canEditAccounts === true && accountSnapshot?.canEdit === true && !pendingAccount && !closeAfterSaves;
  const list = accountSnapshot?.accounts || [];
  const active = list.find(a => a.id === accountSnapshot.activeId);
  document.getElementById("account-management").disabled = document.getElementById("account-management-actions").disabled = !canEdit;
  document.getElementById("account-select-zip").disabled = document.getElementById("account-network").disabled = !canEdit;
  document.getElementById("account-details-toggle").disabled = !active || !!pendingAccount || closeAfterSaves;
  document.getElementById("account-add").disabled = list.length >= 12;
  document.getElementById("account-delete").disabled = list.length <= 1;
  const select = document.getElementById("account-active");
  const signature = JSON.stringify(list);
  if (select.dataset.accounts !== signature) {
    select.replaceChildren();
    list.forEach((account,index) => {
      const option = document.createElement("option"); option.value = String(account.id);
      const duplicate = list.filter(a => a.name === account.name).length > 1;
      option.textContent = account.name + (duplicate ? ` (${index + 1})` : ""); select.append(option);
    });
    select.dataset.accounts = signature;
  }
  if (active) select.value = String(active.id);
  document.getElementById("account-count").textContent = active ? `Account ${list.indexOf(active) + 1} of ${list.length}` : "アカウントを読み込んでいます。";
  document.getElementById("account-lock-note").textContent = state?.phase === "unavailable" ? "Coreがないためアカウント操作は利用できません。" : state?.phase === "running" ? "アカウントの変更はMinecraft終了後に行えます。" : canEdit ? "変更は自動保存されます。アカウントを切り替えてもセーブは移動しません。" : "Coreの準備と保存の完了を待っています。";
  if (accountSnapshot) document.getElementById("account-network").value = accountSnapshot.network;
  const onlineLabels = {ready:"Online Files Ready", "account-credentials-missing":"Account認証情報が不足しています", "online-files-missing":"Online Filesが不足しています", "crypto-not-ready":"Crypto初期化を確認できません", "restart-required":"Online Files Installed · Restart Launcher Required", "custom-missing":"network_services.xmlを選択してください", "online-files-invalid":"Online FilesまたはCustom設定を確認してください"};
  document.getElementById("account-network-status").textContent = `${onlineLabels[accountSnapshot?.onlineStatus] || "確認待ち"} · Runtime Network: ${accountSnapshot?.effectiveNetwork || "Offline"}`;
  document.getElementById("account-custom").hidden = accountSnapshot?.network !== "Custom";
  document.getElementById("account-custom-select").disabled = !canEdit;
  const files = document.getElementById("account-online-files");
  const fileSignature = JSON.stringify(accountSnapshot?.onlineFiles || []);
  if (files.dataset.snapshot !== fileSignature) {
    files.replaceChildren(); files.dataset.snapshot = fileSignature;
    for (const file of accountSnapshot?.onlineFiles || []) { const row = document.createElement("li"); row.textContent = `${file.path} · ${file.state}`; files.append(row); }
    document.getElementById("account-import-mode").value = accountSnapshot?.onlineComplete ? "online" : "account";
    document.getElementById("account-online-replace").checked = false;
  }
  document.querySelector('#account-import-mode option[value="online"]').disabled = !accountSnapshot?.onlineComplete;
  const details = accountSnapshot?.details;
  if (accountDetailsVisible && details && active) {
    document.getElementById("account-details").hidden = false;
    setInfo("account-details", [["Persistent ID",active.id.toString(16).padStart(8,"0")],["Mii Name",active.name],["Account ID",details.accountId || "未設定"],["Birthday",details.birthday || "未設定"],["Gender",details.gender === 0 ? "Male" : details.gender === 1 ? "Female" : "未設定"],["Country / Region Code",details.country || "未設定"],["Email",details.email || "未設定"],["Online Requirements",details.onlineRequirements === "available" ? "必要ファイルあり" : "必要ファイルが不足"]]);
  }
  const candidates = accountSnapshot?.candidates || [];
  const preview = document.getElementById("account-preview");
  preview.hidden = !accountAddDialog.open || !(accountSnapshot?.candidates?.length || accountSnapshot?.previewNotice);
  const container = document.getElementById("account-candidates");
  const previewSignature = JSON.stringify([accountSnapshot?.previewId,candidates]);
  if (container.dataset.snapshot !== previewSignature) {
    container.replaceChildren(); container.dataset.snapshot = previewSignature;
    const labels = {new:"新規 · インポート可能",existing:"既存Account · 対象外",collision:"Persistent ID衝突 · 取込不可",invalid:"不完全または不正 · 取込不可"};
    candidates.forEach(candidate => {
      const row = document.createElement("label"); row.className = "account-candidate";
      const check = document.createElement("input"); check.type = "checkbox"; check.value = String(candidate.id); check.disabled = candidate.state !== "new"; check.checked = !check.disabled;
      const text = document.createElement("span"); text.textContent = `${candidate.name || "未設定"} · Source: ${(candidate.sourceId || candidate.id).toString(16).padStart(8,"0")} · Internal: ${(candidate.internalId || candidate.id).toString(16).padStart(8,"0")} → Destination: ${candidate.id.toString(16).padStart(8,"0")} · ${labels[candidate.state] || "対象外"}`;
      row.append(check,text); container.append(row);
    });
  }
  document.getElementById("account-preview-note").textContent = accountSnapshot?.previewNotice === "preview-ready" ? `${candidates.length} Account検出` : accountSnapshot?.previewNotice === "imported" ? "インポートが完了しました。Active Accountから選択できます。" : accountSnapshot?.previewNotice ? "検証結果: " + accountSnapshot.previewNotice : "";
  document.getElementById("account-console-note").textContent = accountSnapshot?.hasOtp || accountSnapshot?.hasSeeprom ? "本体共通ファイルを検出しました。Account + Online Filesでは表示された認証・System Filesを取り込みます。既存Accountは上書きしません。" : "";
  document.getElementById("account-import").disabled = !canEdit || !candidates.some(c => c.state === "new");
  document.getElementById("account-import-controls").disabled = !canEdit;
  document.getElementById("account-add-cancel").disabled = importInProgress();
  if (!canEdit && accountConfirmationDialog.open) { accountConfirmationAction = null; accountConfirmationDialog.close(); }
  if (state?.canEditAccounts !== true && accountAddDialog.open && !importInProgress()) accountAddDialog.close();
}
window.addEventListener("launcher-account-result", event => {
  if (!pendingAccount || pendingAccount.requestId !== event.detail.requestId) return;
  const command = pendingAccount.command, result = event.detail.account;
  pendingAccount = null;
  if (result.accounts) {
    if (!accountDetailsVisible && !accountEditRequested && !accountEditor.open) delete result.details;
    accountSnapshot = result;
  }
  const message = accountNotices[result.notice] ?? "操作できませんでした。入力と保存先を確認してください。";
  document.getElementById("account-notice").textContent = message;
  document.getElementById("account-editor-note").textContent = message;
  document.getElementById("account-add-notice").textContent = result.previewNotice === "imported"
    ? "インポートが完了しました。Active Accountから選択できます。" + (result.restartRequired ? " Online Files Installed · Restart Launcher Required" : "")
    : message;
  if (command.op === "details" && accountEditRequested && result.details) {
    accountEditRequested = false;
    openAccountEditor();
  }
  if (command.op === "edit" && result.notice !== "saved") {
    accountQueue.length = 0;
    accountEditRequested = true;
    accountQueue.push({op:"details"});
  } else if (["edit","select"].includes(command.op) && accountDetailsVisible && !accountQueue.length) accountQueue.push({op:"details"});
  renderAccount(); drainSaves(); updatePlay(); restoreAccountFocus();
});
document.getElementById("account-active").addEventListener("change", event => {
  hideAccountDetails(); accountCommand({op:"select",id:Number(event.target.value)});
});
document.getElementById("account-details-toggle").addEventListener("click", () => {
  if (accountDetailsVisible) { hideAccountDetails(); return; }
  accountDetailsVisible = true;
  const button = document.getElementById("account-details-toggle"); button.textContent = "Hide Details"; button.setAttribute("aria-expanded","true");
  accountCommand({op:"details"});
});
function openAccountEditor() {
  const active = accountSnapshot?.accounts.find(a => a.id === accountSnapshot.activeId);
  if (!active || !accountSnapshot.details || window.launcherCoreState?.canEditAccounts !== true) return;
  accountEditorId = active.id;
  document.getElementById("account-name").value = active.name;
  document.getElementById("account-editor-note").textContent = "";
  for (const field of ["birthday","gender","country","email"]) document.getElementById("account-"+field).value = accountSnapshot.details[field];
  if (!accountEditor.open) accountEditor.showModal();
  document.getElementById("account-name").focus();
}
document.getElementById("account-edit").addEventListener("click", () => { accountEditRequested = true; accountCommand({op:"details"}); });
document.getElementById("account-editor-cancel").addEventListener("click", () => { accountEditor.close(); if (!accountDetailsVisible && accountSnapshot) delete accountSnapshot.details; });
accountEditor.addEventListener("cancel", () => { if (!accountDetailsVisible && accountSnapshot) delete accountSnapshot.details; });
accountEditor.addEventListener("close", () => {
  for (const field of ["name","birthday","gender","country","email"]) document.getElementById("account-" + field).value = "";
  if (!accountDetailsVisible && accountSnapshot) delete accountSnapshot.details;
  accountEditorId = null;
  accountFocusTarget = document.getElementById("account-edit");
  restoreAccountFocus();
});
document.getElementById("account-editor-form").addEventListener("submit", event => event.preventDefault());
for (const field of ["name","birthday","gender","country","email"]) {
  const control = document.getElementById("account-" + field);
  control.addEventListener("change", () => {
    if (accountEditorId === null || !control.reportValidity()) return;
    accountCommand({op:"edit",id:accountEditorId,field,value:control.value});
  });
}
function confirmAccountAction(action,title,text) {
  if (settingsBusy() || accountConfirmationDialog.open) return;
  accountConfirmationOpener = document.activeElement;
  accountConfirmationAction = action;
  document.getElementById("account-confirmation-title").textContent = title;
  document.getElementById("account-confirmation-text").textContent = text;
  accountConfirmationDialog.showModal(); document.getElementById("account-confirmation-cancel").focus();
}
document.getElementById("account-delete").addEventListener("click", () => {
  const active = accountSnapshot?.accounts.find(a => a.id === accountSnapshot.activeId); if (!active) return;
  confirmAccountAction({op:"delete",id:active.id},"このアカウントを削除しますか？",`${active.name}\nアカウントはバックアップへ退避します。ゲームのセーブは削除しません。`);
});
accountConfirmationDialog.addEventListener("cancel", () => { accountConfirmationAction = null; });
accountConfirmationDialog.addEventListener("close", () => {
  accountFocusTarget = accountConfirmationOpener;
  accountConfirmationOpener = null;
  restoreAccountFocus();
});
document.getElementById("account-confirmation-cancel").addEventListener("click", () => { accountConfirmationDialog.close(); accountConfirmationAction = null; });
document.getElementById("account-confirmation-accept").addEventListener("click", () => {
  const action = accountConfirmationAction; accountConfirmationAction = null; accountConfirmationDialog.close();
  if (action) { if (action.op !== "import") hideAccountDetails(); accountCommand(action); }
});
document.getElementById("account-network").addEventListener("change", event => accountCommand({op:"network",value:event.target.value}));
document.getElementById("account-select-zip").addEventListener("click", () => { if (accountAddDialog.open && !settingsBusy()) accountCommand({op:"select-zip"}); });
document.getElementById("account-import").addEventListener("click", () => {
  const ids = Array.from(document.querySelectorAll("#account-candidates input:checked:not(:disabled)"),e => Number(e.value));
  if (!accountAddDialog.open || settingsBusy() || !ids.length) return;
  const online = document.getElementById("account-import-mode").value === "online";
  const replaceOnline = document.getElementById("account-online-replace").checked;
  if (online && accountSnapshot.onlineFiles.some(f => f.state === "different") && !replaceOnline) { document.getElementById("account-add-notice").textContent = "異なるファイルの置換を明示的に承認してください。"; return; }
  confirmAccountAction({op:"import",preview:accountSnapshot.previewId,ids,online,replaceOnline},"選択したアカウントをインポートしますか？",`${ids.length} Account\n既存Accountとゲームのセーブは上書きしません。${online ? `\nOnline Files: ${accountSnapshot.onlineFiles.filter(f=>f.state !== "identical").length} files · 導入後はLauncherを再起動してください。` : ""}`);
});
const accountDrop = document.getElementById("account-drop");
accountDrop.addEventListener("dragover", event => { event.preventDefault(); if (accountAddDialog.open && !settingsBusy() && window.launcherCoreState?.canEditAccounts === true) { event.dataTransfer.dropEffect = "copy"; accountDrop.dataset.drag = "true"; } });
accountDrop.addEventListener("dragleave", () => { delete accountDrop.dataset.drag; });
accountDrop.addEventListener("drop", event => {
  event.preventDefault(); delete accountDrop.dataset.drag;
  if (!accountAddDialog.open || settingsBusy() || window.launcherCoreState?.canEditAccounts !== true) return;
  const files = event.dataTransfer.files;
  if (files.length !== 1 || !files[0].name.toLowerCase().endsWith(".zip")) { document.getElementById("account-add-notice").textContent = "ZIPファイルを1つ選択してください。"; return; }
  accountCommand({op:"drop",file:files[0]});
});
document.addEventListener("dragover", event => event.preventDefault());
document.addEventListener("drop", event => event.preventDefault());

document.getElementById("account-custom-select").addEventListener("click", () => accountCommand({op:"select-custom",replaceExisting:document.getElementById("account-custom-replace").checked}));
