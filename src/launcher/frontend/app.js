"use strict";

const pages = ["home", "settings"];
const navigation = document.querySelectorAll("[data-page]");
function showPage(selected) {
  if (!pages.includes(selected)) return;
  pages.forEach((page) => { document.getElementById(page).hidden = page !== selected; });
  navigation.forEach((item) => {
    if (item.dataset.page === selected) item.setAttribute("aria-current", "page");
    else item.removeAttribute("aria-current");
  });
  document.getElementById("page-label").textContent = selected === "home" ? "Home" : "Settings";
  document.querySelector("main").scrollTop = 0;
}
navigation.forEach((button) => button.addEventListener("click", () => {
  showPage(button.dataset.page);
  windowCommand("page:" + button.dataset.page);
}));

function windowCommand(command) {
  window.chrome?.webview?.postMessage(command);
}
const play = document.getElementById("play");
play.addEventListener("click", () => {
  if (play.disabled || window.launcherCoreState?.canLaunch !== true) return;
  play.disabled = true;
  windowCommand("game:start");
});
const stopGame = document.getElementById("stop-game");
const stopDialog = document.getElementById("stop-confirmation");
const stopCancel = document.getElementById("stop-cancel");
const stopConfirm = document.getElementById("stop-confirm");
let stopConfirmation = null;
let stopRequestPending = false;
let stopConfirmPending = false;
let restoreStopFocus = false;
function validId(value) {
  return Number.isSafeInteger(value) && value > 0;
}
function cancelStop() {
  if (!stopConfirmation || stopConfirmPending) return;
  const { sessionId, confirmationId } = stopConfirmation;
  stopConfirmation = null;
  restoreStopFocus = true;
  stopDialog.close();
  windowCommand(`game:stop-cancel:${sessionId}:${confirmationId}`);
}
stopGame.addEventListener("click", () => {
  const state = window.launcherCoreState;
  if (stopGame.disabled || state?.canStopGame !== true || !validId(state.sessionId)) return;
  stopRequestPending = true;
  stopGame.disabled = true;
  windowCommand(`game:stop-request:${state.sessionId}`);
});
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
  idle: ["準備中", "ゲームを準備しています", "起動状態を確認しています。"],
  validating: ["準備中", "ゲームを準備しています", "ゲームデータと設定を確認しています。"],
  initializing: ["初期化中", "ゲームを準備しています", "起動の準備を進めています。"],
  ready: ["準備完了", "Minecraft を起動できます", "保存済みのゲーム設定を使用します。"],
  preparing: ["起動中", "Minecraft を起動しています", "ゲームの読み込みを準備しています。"],
  vulkan: ["描画準備中", "Minecraft を起動しています", "Vulkan の描画を準備しています。"],
  running: ["プレイ中", "Minecraft 実行中", "Home でゲームに戻れます。"],
  "title-stopping": ["終了中", "ゲーム終了中", "タイトルの停止を待っています。"],
  "title-stopped": ["終了", "ゲームを終了しました", "次の起動を準備しています。"],
  stopping: ["終了中", "ゲームを終了しています", "終了処理が完了するとランチャーを閉じます。"],
  stopped: ["終了", "ゲームを終了しました", "ランチャーを閉じています。"],
  failed: ["起動失敗", "ゲームを起動できませんでした", "ゲームデータと保存済みの設定を確認してください。"],
  "relaunch-failed": ["再起動失敗", "再起動失敗", "ランチャーを閉じてから、もう一度起動してください。"],
  unavailable: ["起動機能なし", "この画面ではゲームを起動できません", "ランチャーの設定と情報を確認できます。"],
};
window.addEventListener("launcher-core-state", (event) => {
  const state = event.detail;
  window.launcherCoreState = state;
  const phase = state.failed ? (state.sessionId > 1 ? "relaunch-failed" : "failed") : state.stopRequested && !state.stopped ? "stopping" : state.phase;
  const [status, heading, description] = coreLabels[phase] || coreLabels.idle;
  document.querySelector(".status").textContent = status;
  play.disabled = state.canLaunch !== true || settingsBusy() || closeAfterSaves;
  document.querySelector(".launch-panel h2").textContent = heading;
  document.querySelector(".launch-panel p").textContent = description;
  const hasConfirmation = state.canStopGame === true && validId(state.stopConfirmationId) &&
    state.stopConfirmationSessionId === state.sessionId;
  stopRequestPending = false;
  if (hasConfirmation) {
    if (!stopConfirmation || stopConfirmation.confirmationId !== state.stopConfirmationId || stopConfirmation.sessionId !== state.sessionId) {
      stopConfirmation = { sessionId: state.sessionId, confirmationId: state.stopConfirmationId };
      stopConfirmPending = false;
      stopConfirm.disabled = stopCancel.disabled = false;
      if (!stopDialog.open) stopDialog.showModal();
      stopCancel.focus();
    }
  } else {
    const wasOpen = stopDialog.open;
    stopConfirmation = null;
    stopConfirmPending = false;
    if (wasOpen) stopDialog.close();
    stopGame.disabled = state.canStopGame !== true;
    if ((wasOpen || restoreStopFocus) && !stopGame.disabled) stopGame.focus();
    restoreStopFocus = false;
  }
  stopGame.disabled = state.canStopGame !== true || hasConfirmation || stopRequestPending;
  const closing = state.stopRequested === true || phase === "stopping" || phase === "stopped";
  const titleStopping = phase === "title-stopping" || phase === "title-stopped";
  navigation.forEach((button) => { button.disabled = closing || titleStopping || hasConfirmation; });
  document.getElementById("update-preferences").disabled = !confirmed.preferences || closing || titleStopping || hasConfirmation || closeAfterSaves;
  document.querySelectorAll("[data-tab]").forEach((tab) => { tab.disabled = closing || titleStopping || hasConfirmation || closeAfterSaves; });
  document.getElementById("core-information").textContent = status;
  updateNativeSettings(state);
});

const tabs = Array.from(document.querySelectorAll("[data-tab]"));
let selectedTab = "general";
function showTab(name) {
  if (!tabs.some((tab) => tab.dataset.tab === name)) return;
  selectedTab = name;
  tabs.forEach((tab) => {
    const selected = tab.dataset.tab === name;
    tab.setAttribute("aria-selected", String(selected));
    tab.tabIndex = selected ? 0 : -1;
    document.getElementById(tab.getAttribute("aria-controls")).hidden = !selected;
  });
  document.querySelector(".settings-content").scrollTop = 0;
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
function settingsBusy() { return pendingSave !== null || saveQueue.length > 0; }
function same(a, b) { return JSON.stringify(a) === JSON.stringify(b); }
function draft(scope) { return desired[scope] || confirmed[scope]; }
function updatePlay() {
  play.disabled = window.launcherCoreState?.canLaunch !== true || settingsBusy() || closeAfterSaves;
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
  if (pendingSave) return;
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
  if (closeAfterSaves) windowCommand("window:close-confirmed");
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
  closeAfterSaves = true;
  nativeGraphics.disabled = document.getElementById("update-preferences").disabled = true;
  navigation.forEach((button) => { button.disabled = true; });
  tabs.forEach((tab) => { tab.disabled = true; });
  play.disabled = true;
  drainSaves();
});
function setInfo(id, entries) {
  const list = document.getElementById(id);
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
  document.querySelectorAll(".settings-refresh").forEach((button) => {
    button.disabled = state.corePhase !== "ready" || !["ready", "running"].includes(state.phase) || state.stopRequested === true || closeAfterSaves;
  });
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
  const info = state.settingsInfo;
  if (!info?.audioBackend && state.corePhase === "ready" && ["ready", "running"].includes(state.phase) && !requestedSettingsInfo) {
    requestedSettingsInfo = true;
    windowCommand("settings:get");
  }
  if (info?.audioBackend) {
    setInfo("audio-info", [["Audio Backend", info.audioBackend], ["TV Output Device", info.tvDevice], ["TV Volume", info.tvVolume], ["GamePad Output Device", info.padDevice], ["GamePad Volume", info.padVolume]]);
    const connected = (info.controllers || []).filter((device) => device.connected && device.api === "SDLController");
    setInfo("controller-info", [["接続状態", connected.length ? "接続中" : "未接続"], ["接続中のコントローラー", connected.map((device) => device.name).join(" / ") || "なし"], ["VPAD割当", info.vpadAssigned ? "割当済み" : "未割当"], ["Controller Profile", info.profile]]);
  }
}
document.querySelectorAll(".settings-refresh").forEach((button) => button.addEventListener("click", () => windowCommand("settings:get")));
window.addEventListener("launcher-session-ended", () => showPage("home"));
window.addEventListener("launcher-show-home", () => showPage("home"));
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
