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

const scale = document.getElementById("interface-scale");
const scaleOptions = scale.querySelectorAll("input");
const allowedScales = ["1", "1.1"];
function applyScale(value) {
  const accepted = allowedScales.includes(value) ? value : "1";
  scaleOptions.forEach((option) => { option.checked = option.value === accepted; });
  document.documentElement.style.setProperty("--scale", accepted);
}
try { applyScale(localStorage.getItem("interface-scale")); }
catch { applyScale("1"); }
scale.addEventListener("change", (event) => {
  const value = event.target.value;
  if (!allowedScales.includes(value)) return;
  applyScale(value);
  try { localStorage.setItem("interface-scale", value); }
  catch { document.getElementById("settings-note").textContent = "表示サイズは変更しました。保存領域が利用できないため、今回は保存されません。"; }
});

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
  unavailable: ["起動機能なし", "この画面ではゲームを起動できません", "ランチャーの表示設定を変更できます。"],
};
window.addEventListener("launcher-core-state", (event) => {
  const state = event.detail;
  window.launcherCoreState = state;
  const phase = state.failed ? (state.sessionId > 1 ? "relaunch-failed" : "failed") : state.stopRequested && !state.stopped ? "stopping" : state.phase;
  const [status, heading, description] = coreLabels[phase] || coreLabels.idle;
  document.querySelector(".status").textContent = status;
  play.disabled = state.canLaunch !== true;
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
  scale.disabled = closing || titleStopping || hasConfirmation;
  updateNativeSettings(state);
});

const nativeGraphics = document.getElementById("native-graphics");
const nativeFps = document.getElementById("native-fps");
const nativeResolution = document.getElementById("native-resolution");
const settingsRefresh = document.getElementById("settings-refresh");
let nativeDirty = false;
let nativeSaving = false;
let lastNativeSettings = "";
let requestedSettingsInfo = false;
nativeGraphics.addEventListener("change", () => {
  nativeDirty = true;
  document.getElementById("native-settings-note").textContent = "未保存の変更があります。保存すると次回Playで適用されます。";
});
document.getElementById("native-save").addEventListener("click", () => {
  if (nativeGraphics.disabled || window.launcherCoreState?.canEditNativeSettings !== true || nativeSaving) return;
  const [width, height] = nativeResolution.value.split("x").map(Number);
  const payload = { config_version: 1, native_vsync_frequency: Number(nativeFps.value), native_internal_width: width, native_internal_height: height };
  nativeSaving = true;
  nativeGraphics.disabled = true;
  document.getElementById("native-settings-note").textContent = "保存しています。";
  windowCommand("settings:save:" + JSON.stringify(payload));
});
settingsRefresh.addEventListener("click", () => windowCommand("settings:get"));
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
  nativeGraphics.disabled = state.canEditNativeSettings !== true;
  settingsRefresh.disabled = state.corePhase !== "ready" || !["ready", "running"].includes(state.phase) || state.stopRequested === true;
  const settings = state.nativeSettings;
  if (!settings) {
    document.getElementById("native-settings-note").textContent = state.phase === "unavailable" ? "ゲーム設定は、ゲームを起動できるランチャーで変更できます。" : "ゲームの起動準備を待っています。";
    return;
  }
  const signature = JSON.stringify(settings);
  if (signature !== lastNativeSettings || nativeSaving || !nativeDirty || state.canEditNativeSettings !== true) {
    nativeFps.value = String(settings.native_vsync_frequency);
    nativeResolution.value = `${settings.native_internal_width}x${settings.native_internal_height}`;
    nativeDirty = false;
    lastNativeSettings = signature;
  }
  nativeSaving = false;
  const notices = {
    default: "FPS 1000・内部1920×1080を初期値として使用します。保存すると次回Playで適用されます。",
    loaded: "保存済みの設定です。変更は次回Playで適用されます。",
    saved: "設定を保存しました。次回Playで適用されます。",
    "load-failed": "設定を読み込めなかったため、FPS 1000・内部1920×1080を使用します。",
    "save-failed": "設定を保存できませんでした。保存済みの値を表示しています。",
    invalid: "設定値が無効なため、保存しませんでした。",
    "not-ready": "ゲーム停止後、準備完了になると変更できます。",
  };
  const note = nativeDirty ? "未保存の変更があります。保存すると次回Playで適用されます。" : notices[state.settingsNotice] || "ゲームの起動準備を待っています。";
  document.getElementById("native-settings-note").textContent = state.canEditNativeSettings === true ? note : note + " ゲームの実行中・起動中・停止中は変更できません。";
  const info = state.settingsInfo;
  if (!info?.audioBackend && !settingsRefresh.disabled && !requestedSettingsInfo) {
    requestedSettingsInfo = true;
    windowCommand("settings:get");
  }
  if (info?.audioBackend) {
    setInfo("audio-info", [["Audio Backend", info.audioBackend], ["TV出力デバイス", info.tvDevice], ["TV音量", info.tvVolume], ["GamePad出力デバイス", info.padDevice], ["GamePad音量", info.padVolume]]);
    setInfo("controller-info", [["VPAD割当", info.vpadAssigned ? "割当済み" : "未割当"], ["Controller Profile", info.profile], ...(info.controllers || []).flatMap((device) => [["接続状態", device.connected ? "接続中" : "未接続"], ["Input API", device.api], ["デバイス名（保存済み）", device.name]])]);
  }
}
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
