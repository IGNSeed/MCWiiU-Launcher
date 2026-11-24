"use strict";

const pages = ["home", "friends", "world", "texture", "graphic-packs", "session-history", "settings", "setup"];
const navigation = document.querySelectorAll("[data-page]");
const mainContent = document.querySelector("main");
let selectedPage = "home";
let lastLauncherPage = "home";
const pageMotion = new LauncherMotion.PanelTransition(mainContent, pages.map(name => [name,document.getElementById(name)]), null, page => {
  mainContent.dataset.view = page;
  mainContent.scrollTop = 0;
  if (page === "settings") { tabMotion.finish(); tabIndicator.schedule(false); }
  else if (page === "home") {
    LauncherMotion.effect(document.querySelector(".home-art"), [{opacity:.4,transform:"scale(1.01)"},{opacity:1,transform:"scale(1)"}], "slow");
    LauncherMotion.effect(document.querySelector(".home-play"), [{opacity:0},{opacity:1}], "normal", 40);
  }
  LauncherI18n.bind(document.getElementById("page-label"), "textContent", () => page === "friends" ? "Friends" : page === "world" ? "World" : page === "texture" ? "Texture" : page === "graphic-packs" ? "Graphic Packs" : page === "session-history" ? "Session History" : page === "setup" ? "Setup" : "Settings");
  updateInputView(); updateAccountView(); window.updateFriendsView?.(); window.updateWorldView?.(); window.updateSessionHistoryView?.();
}, () => { updateInputView(); updateAccountView(); window.updateFriendsView?.(); window.updateWorldView?.(); window.updateSessionHistoryView?.(); });
const navigationIndicator = new LauncherMotion.SlidingIndicator(document.querySelector("nav"), document.getElementById("navigation-indicator"), [...navigation], () => document.querySelector("nav [aria-current=page]"), true);
function showPage(selected, options = {}) {
  if (!pages.includes(selected)) return Promise.resolve();
  const pageChanged = selected !== selectedPage;
  if (!options.gameOnly) lastLauncherPage = selected;
  const game = selected === "home" && window.launcherCoreState?.phase === "running";
  const target = game ? null : selected;
  if (!pageChanged && pageMotion.target === target && pageMotion.state === "idle") {
    if (options.navigate && !options.gameOnly) windowCommand("page:" + selected);
    return pageMotion.finished;
  }
  stopInputView(); hideAccountDetails(); resetPlayGlow();
  selectedPage = selected;
  if (!options.gameOnly && (pageChanged || options.navigate)) windowCommand("page:" + selected);
  window.updateFriendsView?.();
  window.updateWorldView?.(); window.updateSessionHistoryView?.();
  window.updateTextureView?.();
  window.updateGraphicPacksView?.();
  const focus = document.querySelector(`[data-page="${selected === "setup" ? "home" : selected}"]`);
  if (!game && pages.some(name => document.getElementById(name).contains(document.activeElement))) focus.focus({preventScroll:true});
  navigation.forEach(item => {
    if (item.dataset.page === selected) item.setAttribute("aria-current", "page");
    else item.removeAttribute("aria-current");
  });
  navigationIndicator.schedule(!game && !options.immediate);
  if (selected !== "settings") tabMotion.finish();
  if (game) mainContent.dataset.view = "game";
  return pageMotion.switchTo(target, game || options.immediate);
}
navigation.forEach((button) => button.addEventListener("click", () => {
  showPage(button.dataset.page, {navigate:true});
}));

function windowCommand(command) {
  window.chrome?.webview?.postMessage(command);
}
const play = document.getElementById("play");
let launchPending = false, launchRequestSession = 0;
play.addEventListener("click", () => {
  if (play.disabled || launchPending || settingsBusy()) return;
  if (window.GameDataUI?.canSetup()) { window.GameDataUI.onPlay(); return; }
  if (window.GameDataUI?.busy() || window.launcherCoreState?.canLaunch !== true) return;
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
  idle: ["Preparing"], validating: ["Preparing"], initializing: ["Initializing"], ready: ["Ready"],
  "mii-initializing": ["Initializing Mii Data..."], "mii-committing": ["Initializing Mii Data..."],
  preparing: ["Launching"], opengl: ["Preparing renderer"], vulkan: ["Preparing renderer"], running: ["Playing"],
  "title-stopping": ["Stopping"], "title-stopped": ["Stopped"], stopping: ["Stopping"], stopped: ["Stopped"],
  failed: ["Launch failed"], "relaunch-failed": ["Relaunch failed"], unavailable: ["Launch unavailable"],
};
let startupMessage = null;
function updateStartupMessage() {
  const state = window.launcherCoreState;
  const text = state?.miiInitializing ? LauncherI18n.t(state.miiRestartRequired ? "Mii data initialization could not stop. Restart Launcher." : "Initializing Mii Data...") : "";
  if (text !== startupMessage) { startupMessage = text; windowCommand("language:startup-message:" + JSON.stringify(text)); }
}
window.addEventListener("launcher-language-changed", updateStartupMessage);
let renderedCoreState = "";
window.addEventListener("launcher-core-state", (event) => {
  const state = event.detail;
  window.launcherCoreState = state;
  // 計測値だけの通知では設定や非表示フォームを再描画しない。
  const {fps, presents, glPresents, acquires, images, heldKeys, pendingUiRequests, ...uiState} = state;
  const key = JSON.stringify(uiState);
  if (renderedCoreState === key) return;
  renderedCoreState = key;
  updateStartupMessage();
  const coreError = document.getElementById("core-error");
  LauncherI18n.bind(coreError, "textContent", () => typeof state.error === "string" && state.error ? LauncherI18n.t("Game launch error: ") + LauncherI18n.t(state.error) : "");
  coreError.hidden = !coreError.textContent;
  if (state.phase !== "ready" || state.sessionId !== launchRequestSession || !state.canLaunch) launchPending = false;
  if (state.phase === "running" && selectedPage === "home" && pageMotion.current !== null) showPage("home", {immediate:true});
  const phase = state.failed ? (state.sessionId > 1 ? "relaunch-failed" : "failed") : state.stopRequested && !state.stopped ? "stopping" : state.phase;
  const [status] = coreLabels[phase] || coreLabels.idle;
  updatePlay();
  LauncherI18n.bind(play, "title", () => state.launchUnavailableReason === "game-not-installed" ? LauncherI18n.t("Minecraft BASE and Update v560 are not installed.") : state.launchUnavailableReason === "native-settings-invalid" ? LauncherI18n.t("Cannot read game settings. The original file is preserved.") : "");
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
  document.getElementById("update-preferences").disabled = languageSelect.disabled = !confirmed.preferences || !preferencesReadable || closing || titleStopping || hasConfirmation || closeAfterSaves;
  document.querySelectorAll("[data-tab]").forEach((tab) => { tab.disabled = closing || titleStopping || hasConfirmation || closeAfterSaves; });
  LauncherI18n.bind(document.getElementById("core-information"), "textContent", () => LauncherI18n.t(status));
  updateNativeSettings(state);
  renderAccount();
  updateAccountView();
});

const tabs = Array.from(document.querySelectorAll("[data-tab]"));
let selectedTab = "general";
const tabMotion = new LauncherMotion.PanelTransition(document.querySelector(".settings-content"), tabs.map(tab => [tab.dataset.tab,document.getElementById(tab.getAttribute("aria-controls"))]), "general", () => {
  document.querySelector(".settings-content").scrollTop = 0;
  updateInputView(); updateAccountView();
}, () => { updateInputView(); updateAccountView(); });
const tabIndicator = new LauncherMotion.SlidingIndicator(document.querySelector("#settings .settings-tabs"), document.getElementById("tab-indicator"), tabs, () => tabs.find(tab => tab.dataset.tab === selectedTab));
function showTab(name) {
  if (!tabs.some(tab => tab.dataset.tab === name)) return Promise.resolve();
  if (selectedTab === name && tabMotion.state === "idle") return tabMotion.finished;
  stopInputView(); hideAccountDetails();
  const focused = tabs.some(tab => document.getElementById(tab.getAttribute("aria-controls")).contains(document.activeElement));
  selectedTab = name;
  tabs.forEach(tab => {
    const selected = tab.dataset.tab === name;
    tab.setAttribute("aria-selected", String(selected)); tab.tabIndex = selected ? 0 : -1;
    if (selected && focused) tab.focus({preventScroll:true});
  });
  tabIndicator.schedule(true);
  const settings = document.getElementById("settings");
  return tabMotion.switchTo(name, selectedPage !== "settings" || settings.hidden || settings.inert);
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
const languageSelect = document.getElementById("launcher-language");
const preferenceInputs = ["check-on-startup"].map((id) => document.getElementById(id));
const confirmed = { native: null, preferences: null };
let preferencesReadable = false;
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
  play.disabled = (window.launcherCoreState?.canLaunch !== true && !window.GameDataUI?.canSetup()) || window.GameDataUI?.busy() || launchPending || settingsBusy() || closeAfterSaves || window.launcherUpdateState?.blocksPlay === true;
}
function note(scope, render) {
  LauncherI18n.bind(document.getElementById(scope === "native" ? "native-settings-note" : "preferences-note"), "textContent", render);
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
    note(scope, () => (LauncherI18n.t("Saving…")));
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
  preferenceInputs.forEach((input, index) => { input.checked = values[["check_on_startup"][index]]; });
  languageSelect.value = LauncherI18n.normalize(values.language ?? "auto");
  languageSelect.disabled = !preferencesReadable || closeAfterSaves;
  LauncherI18n.setLanguage(languageSelect.value);
}
function saveFps(value) {
  if (nativeGraphics.disabled || !confirmed.native) return;
  LauncherI18n.bind(fpsError, "textContent", () => "");
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
    LauncherI18n.bind(fpsError, "textContent", () => LauncherI18n.t("Enter an integer from 30 to 1000. Restored the last saved value."));
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
    LauncherI18n.bind(fpsError, "textContent", () => LauncherI18n.t("Enter an integer of up to four digits."));
  }
});
nativeFps.addEventListener("keydown", (event) => {
  if (event.key === "Enter") { event.preventDefault(); commitFps(); }
});
nativeFps.addEventListener("blur", commitFps);
fpsSlider.addEventListener("input", () => { editingFps = true; nativeFps.value = fpsSlider.value; LauncherI18n.bind(fpsError, "textContent", () => ""); });
fpsSlider.addEventListener("change", () => saveFps(Number(fpsSlider.value)));
fpsOriginal.addEventListener("change", () => saveFps(fpsOriginal.checked ? 0 : lastFps));
nativeResolution.addEventListener("change", () => {
  if (nativeGraphics.disabled || !confirmed.native) return;
  const [width, height] = nativeResolution.value.split("x").map(Number);
  enqueueSave("native", { ...draft("native"), native_internal_width: width, native_internal_height: height });
});
preferenceInputs.forEach((input) => input.addEventListener("change", () => {
  enqueueSave("preferences", { ...draft("preferences"), config_version: 3, check_on_startup: preferenceInputs[0].checked });
}));
languageSelect.addEventListener("change", () => {
  if (!preferencesReadable || closeAfterSaves) return;
  LauncherI18n.setLanguage(languageSelect.value);
  enqueueSave("preferences", { ...draft("preferences"), config_version: 3, language: languageSelect.value });
});
window.addEventListener("launcher-preferences", (event) => {
  LauncherI18n.setSystemLanguage(event.detail.systemLanguage);
  confirmed.preferences = event.detail.values;
  LauncherI18n.bind(document.getElementById("launcher-version"), "textContent", () => event.detail.version);
  document.getElementById("update-preferences").disabled = false;
  const healthy = preferencesReadable = ["loaded", "legacy-loaded", "default"].includes(event.detail.notice);
  renderPreferences();
  document.getElementById("update-preferences").disabled = !healthy;
  note("preferences", () => (healthy ? "" : LauncherI18n.t("Cannot read preferences (") + event.detail.notice + LauncherI18n.t("). Existing files are preserved; automatic checks are paused.")));
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
  note(scope, () => (notice === "saved" ? (scope === "native" ? LauncherI18n.t("Saved.") : LauncherI18n.t("Saved.")) : LauncherI18n.t("Could not save. Restored the saved values. Try again.")));
  if (scope === "native") renderNative(); else renderPreferences();
  drainSaves();
});
window.addEventListener("launcher-close-request", (event) => {
  const finish = () => {
    commitFps();
    stopInputView();
    closeAfterSaves = true;
    nativeGraphics.disabled = document.getElementById("update-preferences").disabled = true;
    navigation.forEach((button) => { button.disabled = true; });
    tabs.forEach((tab) => { tab.disabled = true; });
    play.disabled = true;
    drainSaves();
  };
  if (window.GameDataUI) window.GameDataUI.beforeClose(event, finish); else finish();
});
function setInfo(id, entries) {
  const list = document.getElementById(id);
  const key = LauncherI18n.language + JSON.stringify(entries);
  if (list.dataset.snapshot === key) return;
  list.dataset.snapshot = key;
  list.replaceChildren();
  entries.forEach(([label, value]) => {
    const term = document.createElement("dt"), detail = document.createElement("dd");
    LauncherI18n.bind(term, "textContent", () => label);
    LauncherI18n.bind(detail, "textContent", () => String(value));
    list.append(term, detail);
  });
}
function updateNativeSettings(state) {
  nativeGraphics.disabled = state.canEditNativeSettings !== true || closeAfterSaves;
  if (!state.nativeSettings) {
    note("native", () => (state.phase === "unavailable" ? LauncherI18n.t("Game settings require Cemu Core.") : LauncherI18n.t("Preparing…")));
    setInfo("audio-info", [[LauncherI18n.t("Current settings"), LauncherI18n.t("Core information unavailable.")]]);
    setInfo("controller-info", [[LauncherI18n.t("Connection"), LauncherI18n.t("Core information unavailable.")]]);
    return;
  }
  confirmed.native = state.nativeSettings;
  if (state.nativeSettings.native_vsync_frequency) lastFps = state.nativeSettings.native_vsync_frequency;
  if (state.canEditNativeSettings !== true) editingFps = false;
  renderNative();
  if (!pendingSave && !saveQueue.includes("native")) {
    const notices = { get default() { return ""; }, get loaded() { return ""; }, get saved() { return LauncherI18n.t("Saved."); }, get "load-failed"() { return LauncherI18n.t("Could not load settings. Using 1000 FPS and 1920×1080."); }, get "save-failed"() { return LauncherI18n.t("Could not save. Showing saved values."); }, get invalid() { return LauncherI18n.t("Invalid settings were not saved."); }, get "not-ready"() { return LauncherI18n.t("Available after the game stops."); } };
    note("native", () => ((notices[state.settingsNotice] ?? LauncherI18n.t("Preparing…")) + (state.canEditNativeSettings === true ? "" : LauncherI18n.t(" Available after the game stops."))));
  }
  renderInput(state.settingsInfo?.input, state);
  renderAv(state.settingsInfo?.av, state);
  const info = state.settingsInfo;
  if (!info?.audioBackend && state.corePhase === "ready" && ["ready", "running"].includes(state.phase) && !requestedSettingsInfo) {
    requestedSettingsInfo = true;
    windowCommand("settings:get");
  }
  if (info?.audioBackend) {
    const outputName = (key, fallback) => {
      const token = info.av?.values[key];
      const device = info.av?.outputs.find(device => device.token === token);
      return token === 0 ? LauncherI18n.t("Disabled") : device?.name === "Default" ? LauncherI18n.t("Default") : device?.name || fallback;
    };
    setInfo("audio-info", [[LauncherI18n.t("Audio Backend"), info.audioBackend], [LauncherI18n.t("TV Output Device"), outputName("tvDevice", info.tvDevice)], [LauncherI18n.t("TV Volume"), info.tvVolume], [LauncherI18n.t("GamePad Output Device"), outputName("padDevice", info.padDevice)], [LauncherI18n.t("GamePad Volume"), info.padVolume]]);
    const connected = (info.controllers || []).filter((device) => device.connected && device.api === "SDLController");
    if (!info.input?.mapping) setInfo("controller-info", [[LauncherI18n.t("Connection"), connected.length ? LauncherI18n.t("Connected") : LauncherI18n.t("Disconnected")], [LauncherI18n.t("Connected controller"), connected.map((device) => device.name).join(" / ") || LauncherI18n.t("None")], [LauncherI18n.t("VPAD assignment"), info.vpadAssigned ? LauncherI18n.t("Assigned") : LauncherI18n.t("Unassigned")], [LauncherI18n.t("Controller Profile"), info.profile]]);
  }
}

window.addEventListener("launcher-session-ended", () => showPage(lastLauncherPage));
window.addEventListener("launcher-show-home", () => showPage("home"));
window.addEventListener("launcher-return-game", () => showPage("home", {immediate:true,gameOnly:true}));
window.addEventListener("launcher-display-state", (event) => {
  window.launcherDisplayState = event.detail;
  if (!event.detail.webViewVisible) LauncherMotion.finish();
});
document.getElementById("window-minimize").addEventListener("click", () => windowCommand("window:minimize"));
document.getElementById("window-maximize").addEventListener("click", () => windowCommand("window:toggle-maximize"));
document.getElementById("window-close").addEventListener("click", () => windowCommand("window:close"));
window.addEventListener("launcher-window-state", (event) => {
  const maximized = event.detail === true;
  document.documentElement.dataset.maximized = String(maximized);
  const button = document.getElementById("window-maximize");
  const label = maximized ? LauncherI18n.t("Restore") : LauncherI18n.t("Maximize");
  LauncherI18n.bind(button, "aria-label", () => label, true);
  LauncherI18n.bind(button, "title", () => label);
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
let renderedLiveInput = "";
window.addEventListener("resize", () => { renderedLiveInput = ""; });
const inputMessages = {
  get ready() { return ""; },
  get saved() { return LauncherI18n.t("Profile saved and verified."); },
  get "saved-duplicate"() { return LauncherI18n.t("Saved. Some mappings share the same physical input."); },
  get "save-failed"() { return LauncherI18n.t("Could not save. Restored the previous settings. Try again."); },
  get "recovery-required"() { return LauncherI18n.t("Profile recovery required. Preserve backups and close the launcher."); },
  get "stale-request"() { return LauncherI18n.t("Stale action ignored. Check the latest settings."); },
  get rejected() { return LauncherI18n.t("Could not apply. Check the profile name, duplicate names and controller connection."); },
  get "not-ready"() { return LauncherI18n.t("Unavailable during startup or shutdown."); },
  get "release-inputs"() { return LauncherI18n.t("Release all buttons and sticks within 10 seconds."); },
  get "press-input"() { return LauncherI18n.t("Press a button, or move a stick or trigger fully."); },
  get "capture-timeout"() { return LauncherI18n.t("Input capture timed out. Select a mapping to try again."); },
  get "capture-cancelled"() { return LauncherI18n.t("Input capture cancelled."); },
  get disconnected() { return LauncherI18n.t("Disconnected. Check the connection and select a controller."); },
  get reconnected() { return LauncherI18n.t("Controller reconnected. Saved mappings restored."); },
  get "connected-new-model"() { return LauncherI18n.t("New controller connected. Using its mappings; the previous profile is preserved."); },
  get "duplicate-input"() { return LauncherI18n.t("This input is already mapped. Release it and press another button."); },
  get "select-controller"() { return LauncherI18n.t("Select a connected controller."); },
  get "unsupported-profile"() { return LauncherI18n.t("Select a physical controller to create SDLController mappings. The current profile is preserved."); },
};
function inputActive() { return selectedPage === "settings" && !document.getElementById("settings").hidden && !document.getElementById("settings").inert && selectedTab === "input" && tabMotion.current === "input" && !document.getElementById("panel-input").inert && !document.hidden && !closeAfterSaves; }
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
  if (!pendingInput) LauncherI18n.bind(inputNote, "textContent", () => editable ? (inputMessages[current.notice] || inputMessages.ready) : inputMessages["not-ready"]);
  options(physicalController, [["", LauncherI18n.t("Select controller")], ...current.devices.map((d) => [String(d.token), d.name + (current.devices.filter(other => other.name === d.name).length > 1 ? ` (${current.devices.filter(other => other.name === d.name).indexOf(d) + 1})` : "")])], String(current.devices.find((d) => d.selected)?.token || ""));
  const chosenProfile = profileSelect.value;
  options(profileSelect, [["", LauncherI18n.t("Select profile")], ...current.profiles.map((name) => [name, name])], current.profiles.includes(chosenProfile) ? chosenProfile : current.profiles.includes(current.profile) ? current.profile : "");
  setInfo("controller-info", [[LauncherI18n.t("Controller Status"), current.connected ? LauncherI18n.t("Connected") : LauncherI18n.t("Disconnected")], [LauncherI18n.t("Controller Profile"), current.profile], [LauncherI18n.t("Player"), "1 · Wii U GamePad"]]);
  for (const [side, value] of [["left", current.leftDeadzone], ["right", current.rightDeadzone]]) {
    const slider = document.getElementById(side + "-deadzone");
    if (document.activeElement !== slider || !editable || ["save-failed","rejected"].includes(current.notice)) {
      slider.value = String(value);
      document.getElementById(side + "-deadzone-value").value = Number(value).toFixed(2);
    }
  }
  updateDeadzonePreview();
  const key = LauncherI18n.language + JSON.stringify(current.mapping);
  if (mappingGrid.dataset.snapshot !== key) {
    renderedLiveInput = "";
    mappingGrid.dataset.snapshot = key;
    mappingGrid.replaceChildren(...current.mapping.map((m) => {
      const row = document.createElement("div"), label = document.createElement("span"), button = document.createElement("button");
      row.className = "mapping-row"; row.dataset.mapping = m.id; row.dataset.button = m.button ?? "";
      LauncherI18n.bind(label, "textContent", () => LauncherI18n.t(m.label)); LauncherI18n.bind(button, "textContent", () => (m.name || LauncherI18n.t("Unassigned")) + (m.duplicate ? LauncherI18n.t(" (duplicate)") : ""));
      button.type = "button"; LauncherI18n.bind(button, "aria-label", () => LauncherI18n.t(m.label) + LauncherI18n.t(" mapping: ") + (m.name || LauncherI18n.t("Unassigned")), true);
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
captureCancel.addEventListener("click", () => { windowCommand("input:cancel"); lastCaptureId = 0; document.getElementById("capture-actions").hidden = true; LauncherI18n.bind(inputNote, "textContent", () => inputMessages["capture-cancelled"]); });
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
  if (event.detail.input.notice) LauncherI18n.bind(inputNote, "textContent", () => inputMessages[event.detail.input.notice] || inputMessages.ready);
  drainSaves(); updatePlay();
});
function clearLiveInput() {
  renderedLiveInput = "";
  for (const side of ["left", "right"]) {
    document.querySelectorAll("#" + side + "-stick-test span").forEach(point => point.style.transform = "translate(-50%,-50%)");
    document.getElementById(side + "-stick-value").value = LauncherI18n.t("Raw 0.00 / 0.00 · Output 0.00 / 0.00");
  }
  mappingGrid.querySelectorAll(".mapping-row").forEach((row) => { row.dataset.pressed = "false"; });
  LauncherI18n.bind(document.getElementById("trigger-test"), "textContent", () => LauncherI18n.t("ZL 0.00 / ZR 0.00"));
  LauncherI18n.bind(document.getElementById("button-test"), "textContent", () => LauncherI18n.t("Disconnected"));
}
window.addEventListener("launcher-input-live", (event) => {
  if (!inputActive() || event.detail.viewId !== inputViewId) return;
  inputPollPending = false;
  const state = event.detail.state;
  const key = LauncherI18n.language + JSON.stringify(state);
  if (renderedLiveInput === key) return;
  if (!state.connected) { clearLiveInput(); renderedLiveInput = key; }
  else {
    renderedLiveInput = key;
    for (const side of ["left", "right"]) {
      const [x,y] = state[side], raw = state[side === "left" ? "rawLeft" : "rawRight"];
      const test = document.getElementById(side + "-stick-test"), radius = test.clientWidth / 2 - 5;
      for (const [selector, values] of [[".raw-point",raw],[".processed-point",[x,y]]])
        test.querySelector(selector).style.transform = `translate(calc(-50% + ${values[0] * radius}px),calc(-50% + ${values[1] * radius}px))`;
      document.getElementById(side + "-stick-value").value = LauncherI18n.t("Raw {0} / {1} · Output {2} / {3}", [raw[0].toFixed(2), raw[1].toFixed(2), x.toFixed(2), y.toFixed(2)]);
    }
    LauncherI18n.bind(document.getElementById("trigger-test"), "textContent", () => LauncherI18n.t("ZL {0} / ZR {1}", [state.triggers[0].toFixed(2), state.triggers[1].toFixed(2)]));
    LauncherI18n.bind(document.getElementById("button-test"), "textContent", () => state.buttons.map((id) => inputSnapshot?.buttons?.find((b) => b.id === id)?.name || LauncherI18n.t("Button {0}", [id])).join(" · ") || LauncherI18n.t("Buttons released"));
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
  if (state.captureId || lastCaptureId) LauncherI18n.bind(inputNote, "textContent", () => (state.captureId ? (inputSnapshot?.mapping.find(m => m.id === state.captureMapping)?.label || "") + " · " : "") + (inputMessages[state.notice] || inputMessages.ready));
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
  if (selectedPage !== "settings" || document.getElementById("settings").hidden || document.getElementById("settings").inert || document.hidden || closeAfterSaves) return;
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
const avLabels = ["DirectSound",LauncherI18n.t("XAudio2 2.7"),"XAudio2","Cubeb"];
function avNote(key,render) { LauncherI18n.bind(document.getElementById(avCategory(key) + "-settings-note"), "textContent", render); }
function renderAv(info, state = window.launcherCoreState) {
  if (info?.values) avSnapshot = info;
  const current = avSnapshot;
  for (const [id,key] of [["av-graphics","graphicsApi"],["av-audio","audioApi"],["av-overlay","overlayPosition"]]) document.getElementById(id).disabled = !canEditAv(key,state) || closeAfterSaves || pendingAv !== null || !!state?.stopConfirmationId;
  if (!current) return;
  const values = current.values;
  const select = (key, items) => {
    const value = String(values[key]);
    if (!items.some(item => String(item[0]) === value)) items.push([value,LauncherI18n.t("Saved device (unavailable)")]);
    options(document.getElementById("av-" + key),items,value);
    for (const option of document.getElementById("av-" + key).options) option.disabled = Number(option.value) < 0;
  };
  if (values.graphicsApi === 0) options(document.getElementById("av-gpu"),[[0,LauncherI18n.t("System Default")]],"0");
  else select("gpu", [[0,LauncherI18n.t("Default")], ...current.gpus.map(d => [d.token,d.name === "Default" ? LauncherI18n.t("Default") : d.name])]);
  select("vsync", values.graphicsApi === 0 ? [[0,LauncherI18n.t("Off")],[1,LauncherI18n.t("On")]] : [[0,LauncherI18n.t("Off")],[1,LauncherI18n.t("Double Buffering")],[2,LauncherI18n.t("Triple Buffering")],[3,LauncherI18n.t("Match Emulated Display (Experimental)")]]);
  select("audioApi",current.audioApis.map(api => [api,avLabels[api]]));
  for (const key of ["tvDevice","padDevice","micDevice"])
    select(key,[[0,LauncherI18n.t("Disabled")], ...(key === "micDevice" ? current.inputs : current.outputs).map(d => [d.token,d.name === "Default" ? LauncherI18n.t("Default") : d.name])]);
  select("tvChannels",current.tvChannels.map(channel => [channel,[LauncherI18n.t("Mono"),LauncherI18n.t("Stereo"),LauncherI18n.t("Surround")][channel]]));
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
  LauncherI18n.bind(document.getElementById("renderer-info"), "textContent", () => state?.rendererDevice ? LauncherI18n.t("Current renderer: {0} · {1}", [state.activeRenderer, state.rendererDevice]) : "");
  const missing = ["gpu","tvDevice","padDevice","micDevice"].filter(key => values[key] === -1);
  if (!pendingAv && current.notice !== "save-failed" && current.notice !== "rejected") {
    LauncherI18n.bind(document.getElementById("graphics-settings-note"), "textContent", () => missing.includes("gpu") ? LauncherI18n.t("Saved GPU unavailable. The selection is preserved; launch uses the default GPU.") : "");
    LauncherI18n.bind(document.getElementById("audio-settings-note"), "textContent", () => missing.some(key => key !== "gpu") ? LauncherI18n.t("Saved audio device unavailable. The selection is preserved. Reconnect or choose a device.") : current.enumerationNotice ? LauncherI18n.t("Could not list devices. Check the connection.") : "");
    LauncherI18n.bind(document.getElementById("overlay-settings-note"), "textContent", () => "");
  }
  LauncherI18n.bind(document.getElementById("audio-stream-note"), "textContent", () => state?.phase === "running" ? LauncherI18n.t("TV Stream: {0} · GamePad Stream: {1}", [current.tvStream ? LauncherI18n.t("Created") : values.tvDevice === 0 ? LauncherI18n.t("Disabled") : LauncherI18n.t("Not initialized; check output"), current.padStream ? LauncherI18n.t("Created") : values.padDevice === 0 ? LauncherI18n.t("Disabled") : LauncherI18n.t("Not initialized")]) : "");
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
  avNote(pendingAv.key,() => (LauncherI18n.t("Applying and saving…")));
  windowCommand("av:command:" + pendingAv.requestId + ":" + JSON.stringify({key:pendingAv.key,value:pendingAv.value,revision:avSnapshot.revision}));
  renderAv(); updatePlay();
}
for (const key of avKeys) {
  const control = document.getElementById("av-" + key);
  if (control.type === "color") {
    const hex = document.getElementById(control.id + "-hex"), alpha = document.getElementById(control.id + "-alpha");
    const commit = value => {
      const opacity = Number(alpha.value);
      if (alpha.value === "" || !/^#[0-9A-Fa-f]{6}$/.test(value) || !Number.isInteger(opacity) || opacity < 0 || opacity > 255) { avNote(key,() => (LauncherI18n.t("Enter #RRGGBB and alpha from 0 to 255. Invalid values are not saved."))); return; }
      hex.setCustomValidity(""); control.value = hex.value = value.toUpperCase(); avCommand(key,colorRgba(value,opacity));
    };
    control.addEventListener("change", () => commit(control.value));
    hex.addEventListener("change", () => commit(hex.value));
    hex.addEventListener("input", () => hex.setCustomValidity(/^#[0-9A-Fa-f]{6}$/.test(hex.value) ? "" : LauncherI18n.t("Enter #RRGGBB.")));
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
  avNote(key,() => (result.notice === "saved" ? (avCategory(key) === "graphics" ? LauncherI18n.t("Saved.") : LauncherI18n.t("Applied · Saved")) : result.notice === "recovery-required" ? LauncherI18n.t("Settings recovery required. Close the launcher.") : LauncherI18n.t("Could not save. Restored saved values. Try again.")));
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
  LauncherI18n.bind(document.getElementById("account-add-notice"), "textContent", () => "");
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
  if (selectedPage !== "settings" || selectedTab !== "account" || document.getElementById("settings").hidden || document.getElementById("settings").inert || document.getElementById("panel-account").inert || window.launcherCoreState?.canEditAccounts !== true) return;
  const modal = document.querySelector("dialog[open]");
  if (modal && !modal.contains(target)) return;
  if (target.matches(":disabled")) target = modal ? document.getElementById("account-add-cancel") : document.getElementById("account-active");
  target.focus({preventScroll:true});
}
const accountEditor = document.getElementById("account-editor");
const accountConfirmationDialog = document.getElementById("account-confirmation");
const accountNotices = {
  get saved() { return LauncherI18n.t("Saved."); }, ready: "", get "preview-ready"() { return LauncherI18n.t("ZIP verified. Review the selected accounts before importing."); },
  get "preview-failed"() { return LauncherI18n.t("Could not import ZIP. Check its structure, size and integrity."); }, get "not-ready"() { return LauncherI18n.t("Account changes are available after Minecraft stops."); },
  get "last-account"() { return LauncherI18n.t("The last account cannot be deleted."); }, get "account-limit"() { return LauncherI18n.t("Up to 12 accounts are allowed."); }, get "invalid-name"() { return LauncherI18n.t("Enter a nonblank Mii name of up to 10 characters."); },
  get "invalid-birthday"() { return LauncherI18n.t("Enter a valid birthday."); }, get "invalid-email"() { return LauncherI18n.t("Enter a valid email address."); }, get "invalid-value"() { return LauncherI18n.t("Check the value range."); },
  get "stale-revision"() { return LauncherI18n.t("Settings changed. Review the latest values and retry."); }, get "stale-preview"() { return LauncherI18n.t("Refresh the preview."); },
  get "persistent-id-exists"() { return LauncherI18n.t("Persistent ID already exists. Nothing was overwritten."); }, get "id-unavailable"() { return LauncherI18n.t("Could not allocate an account ID."); },
  get "save-failed"() { return LauncherI18n.t("Could not save. Restored the original data."); }, get "recovery-required"() { return LauncherI18n.t("Recovery required. Close the launcher and check the backup."); },
  get "preferences-invalid"() { return LauncherI18n.t("Cannot read network service settings. Using Offline."); }
};
function hideAccountDetails() {
  accountDetailsVisible = accountEditRequested = false;
  if (accountSnapshot) delete accountSnapshot.details;
  const details = document.getElementById("account-details");
  details.hidden = true; details.replaceChildren(); delete details.dataset.snapshot;
  const button = document.getElementById("account-details-toggle");
  LauncherI18n.bind(button, "textContent", () => LauncherI18n.t("Show Details")); button.setAttribute("aria-expanded", "false");
  if (accountEditor.open) accountEditor.close();
  if (accountAddDialog.open && !importInProgress()) accountAddDialog.close();
  if (accountConfirmationDialog.open) accountConfirmationDialog.close();
  document.getElementById("account-email").value = document.getElementById("account-birthday").value = "";
  accountEditorId = null;
}
function updateAccountView() {
  if (selectedPage !== "settings" || selectedTab !== "account" || tabMotion.current !== "account" || document.getElementById("settings").hidden || document.getElementById("settings").inert || document.getElementById("panel-account").inert || pendingAccount || accountQueue.length) return;
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
      LauncherI18n.bind(document.getElementById("account-notice"), "textContent", () => LauncherI18n.t("Drag and drop unavailable. Use Select ZIP File."));
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
      LauncherI18n.bind(option, "textContent", () => account.name + (duplicate ? ` (${index + 1})` : "")); select.append(option);
    });
    select.dataset.accounts = signature;
  }
  if (active) select.value = String(active.id);
  LauncherI18n.bind(document.getElementById("account-count"), "textContent", () => active ? LauncherI18n.t("Account {0} of {1}", [list.indexOf(active) + 1, list.length]) : LauncherI18n.t("Loading accounts…"));
  LauncherI18n.bind(document.getElementById("account-lock-note"), "textContent", () => state?.phase === "unavailable" ? LauncherI18n.t("Account management requires Cemu Core.") : state?.phase === "running" ? LauncherI18n.t("Account changes are available after Minecraft stops.") : canEdit ? LauncherI18n.t("Switching accounts does not move saves.") : LauncherI18n.t("Waiting for Core and pending saves…"));
  if (accountSnapshot) document.getElementById("account-network").value = accountSnapshot.network;
  const onlineLabels = {get ready() { return LauncherI18n.t("Online Files Ready"); }, get "account-credentials-missing"() { return LauncherI18n.t("Account credentials missing"); }, get "online-files-missing"() { return LauncherI18n.t("Online Files missing"); }, get "crypto-not-ready"() { return LauncherI18n.t("Crypto initialization unconfirmed"); }, get "restart-required"() { return LauncherI18n.t("Online Files Installed · Restart Launcher Required"); }, get "custom-missing"() { return LauncherI18n.t("Select network_services.xml"); }, get "online-files-invalid"() { return LauncherI18n.t("Check Online Files and custom settings"); }};
  LauncherI18n.bind(document.getElementById("account-network-status"), "textContent", () => `${onlineLabels[accountSnapshot?.onlineStatus] || LauncherI18n.t("Pending")} · ${LauncherI18n.t("Network Service")}: ${LauncherI18n.t(accountSnapshot?.effectiveNetwork || "Offline")}`);
  document.getElementById("account-custom").hidden = accountSnapshot?.network !== "Custom";
  document.getElementById("account-custom-select").disabled = !canEdit;
  const files = document.getElementById("account-online-files");
  const fileSignature = JSON.stringify(accountSnapshot?.onlineFiles || []);
  if (files.dataset.snapshot !== fileSignature) {
    files.replaceChildren(); files.dataset.snapshot = fileSignature;
    for (const file of accountSnapshot?.onlineFiles || []) { const row = document.createElement("li"); LauncherI18n.bind(row, "textContent", () => `${file.path} · ${file.state}`); files.append(row); }
    document.getElementById("account-import-mode").value = accountSnapshot?.onlineComplete ? "online" : "account";
    document.getElementById("account-online-replace").checked = false;
  }
  document.querySelector('#account-import-mode option[value="online"]').disabled = !accountSnapshot?.onlineComplete;
  const details = accountSnapshot?.details;
  if (accountDetailsVisible && details && active) {
    document.getElementById("account-details").hidden = false;
    setInfo("account-details", [[LauncherI18n.t("Persistent ID"),active.id.toString(16).padStart(8,"0")],[LauncherI18n.t("Mii Name"),active.name],[LauncherI18n.t("Account ID"),details.accountId || LauncherI18n.t("Not set")],[LauncherI18n.t("Birthday"),details.birthday || LauncherI18n.t("Not set")],[LauncherI18n.t("Gender"),details.gender === 0 ? LauncherI18n.t("Male") : details.gender === 1 ? LauncherI18n.t("Female") : LauncherI18n.t("Not set")],[LauncherI18n.t("Country / Region Code"),details.country || LauncherI18n.t("Not set")],[LauncherI18n.t("Email"),details.email || LauncherI18n.t("Not set")],[LauncherI18n.t("Online Requirements"),details.onlineRequirements === "available" ? LauncherI18n.t("Required files available") : LauncherI18n.t("Required files missing")]]);
  }
  const candidates = accountSnapshot?.candidates || [];
  const preview = document.getElementById("account-preview");
  preview.hidden = !accountAddDialog.open || !(accountSnapshot?.candidates?.length || accountSnapshot?.previewNotice);
  const container = document.getElementById("account-candidates");
  const previewSignature = LauncherI18n.language + JSON.stringify([accountSnapshot?.previewId,candidates]);
  if (container.dataset.snapshot !== previewSignature) {
    container.replaceChildren(); container.dataset.snapshot = previewSignature;
    const labels = {get new() { return LauncherI18n.t("New · Ready to import"); },get existing() { return LauncherI18n.t("Existing account · Excluded"); },get collision() { return LauncherI18n.t("Persistent ID conflict · Cannot import"); },get invalid() { return LauncherI18n.t("Invalid or incomplete · Cannot import"); }};
    candidates.forEach(candidate => {
      const row = document.createElement("label"); row.className = "account-candidate";
      const check = document.createElement("input"); check.type = "checkbox"; check.value = String(candidate.id); check.disabled = candidate.state !== "new"; check.checked = !check.disabled;
      const text = document.createElement("span"); LauncherI18n.bind(text, "textContent", () => `${candidate.name || LauncherI18n.t("Not set")} · Source: ${(candidate.sourceId || candidate.id).toString(16).padStart(8,"0")} · Internal: ${(candidate.internalId || candidate.id).toString(16).padStart(8,"0")} → Destination: ${candidate.id.toString(16).padStart(8,"0")} · ${labels[candidate.state] || LauncherI18n.t("Excluded")}`);
      row.append(check,text); container.append(row);
    });
  }
  LauncherI18n.bind(document.getElementById("account-preview-note"), "textContent", () => accountSnapshot?.previewNotice === "preview-ready" ? LauncherI18n.t("{0} accounts found", [candidates.length]) : accountSnapshot?.previewNotice === "imported" ? LauncherI18n.t("Accounts imported.") : accountSnapshot?.previewNotice ? LauncherI18n.t("Validation result: ") + accountSnapshot.previewNotice : "");
  LauncherI18n.bind(document.getElementById("account-console-note"), "textContent", () => accountSnapshot?.hasOtp || accountSnapshot?.hasSeeprom ? LauncherI18n.t("Console files found. Account + Online Files imports the listed authentication and system files. Existing accounts are not overwritten.") : "");
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
  const message = () => accountNotices[result.notice] ?? LauncherI18n.t("Could not complete the action. Check the input and storage location.");
  if (!["clear-preview","list","details"].includes(command.op) || (result.notice !== "saved" && message())) LauncherI18n.bind(document.getElementById("account-notice"), "textContent", () => message());
  LauncherI18n.bind(document.getElementById("account-editor-note"), "textContent", () => message());
  LauncherI18n.bind(document.getElementById("account-add-notice"), "textContent", () => result.previewNotice === "imported"
    ? LauncherI18n.t("Accounts imported.") + (result.restartRequired ? " " + LauncherI18n.t("Online Files Installed · Restart Launcher Required") : "")
    : message());
  if (command.op === "details" && accountEditRequested && result.details) {
    accountEditRequested = false;
    openAccountEditor();
  }
  if (command.op === "edit" && result.notice !== "saved") {
    accountQueue.length = 0;
    accountEditRequested = true;
    accountQueue.push({op:"details"});
  } else if (["edit","select"].includes(command.op) && accountDetailsVisible && !accountQueue.length) accountQueue.push({op:"details"});
  if (command.op === "import" && result.notice === "saved" && result.previewNotice === "imported") {
    LauncherI18n.bind(document.getElementById("account-notice"), "textContent", () => result.restartRequired
      ? LauncherI18n.t("Accounts imported. Restart the launcher to use Online Files.")
      : LauncherI18n.t("Accounts imported."));
    if (accountAddDialog.open) accountAddDialog.close();
    showPage("settings"); showTab("account");
  }
  renderAccount(); drainSaves(); updatePlay(); restoreAccountFocus();
});
document.getElementById("account-active").addEventListener("change", event => {
  hideAccountDetails(); accountCommand({op:"select",id:Number(event.target.value)});
});
document.getElementById("account-details-toggle").addEventListener("click", () => {
  if (accountDetailsVisible) { hideAccountDetails(); return; }
  accountDetailsVisible = true;
  const button = document.getElementById("account-details-toggle"); LauncherI18n.bind(button, "textContent", () => LauncherI18n.t("Hide Details")); button.setAttribute("aria-expanded","true");
  accountCommand({op:"details"});
});
function openAccountEditor() {
  const active = accountSnapshot?.accounts.find(a => a.id === accountSnapshot.activeId);
  if (!active || !accountSnapshot.details || window.launcherCoreState?.canEditAccounts !== true) return;
  accountEditorId = active.id;
  document.getElementById("account-name").value = active.name;
  LauncherI18n.bind(document.getElementById("account-editor-note"), "textContent", () => "");
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
  LauncherI18n.bind(document.getElementById("account-confirmation-title"), "textContent", () => title);
  LauncherI18n.bind(document.getElementById("account-confirmation-text"), "textContent", () => text);
  accountConfirmationDialog.showModal(); document.getElementById("account-confirmation-cancel").focus();
}
document.getElementById("account-delete").addEventListener("click", () => {
  const active = accountSnapshot?.accounts.find(a => a.id === accountSnapshot.activeId); if (!active) return;
  confirmAccountAction({op:"delete",id:active.id},LauncherI18n.t("Delete this account?"),LauncherI18n.t("{0}\nThe account will be backed up. Game saves will not be deleted.", [active.name]));
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
  if (online && accountSnapshot.onlineFiles.some(f => f.state === "different") && !replaceOnline) { LauncherI18n.bind(document.getElementById("account-add-notice"), "textContent", () => LauncherI18n.t("Approve replacement of the different files to continue.")); return; }
  confirmAccountAction({op:"import",preview:accountSnapshot.previewId,ids,online,replaceOnline},LauncherI18n.t("Import selected accounts?"),LauncherI18n.t("{0} accounts\nExisting accounts and game saves will not be overwritten.{1}", [ids.length, online ? LauncherI18n.t("\nOnline Files: {0} files · Restart the launcher after importing.", [accountSnapshot.onlineFiles.filter(f=>f.state !== "identical").length]) : ""]));
});
const accountDrop = document.getElementById("account-drop");
accountDrop.addEventListener("dragover", event => { event.preventDefault(); if (accountAddDialog.open && !settingsBusy() && window.launcherCoreState?.canEditAccounts === true) { event.dataTransfer.dropEffect = "copy"; accountDrop.dataset.drag = "true"; } });
accountDrop.addEventListener("dragleave", () => { delete accountDrop.dataset.drag; });
accountDrop.addEventListener("drop", event => {
  event.preventDefault(); delete accountDrop.dataset.drag;
  if (!accountAddDialog.open || settingsBusy() || window.launcherCoreState?.canEditAccounts !== true) return;
  const files = event.dataTransfer.files;
  if (files.length !== 1 || !files[0].name.toLowerCase().endsWith(".zip")) { LauncherI18n.bind(document.getElementById("account-add-notice"), "textContent", () => LauncherI18n.t("Select one ZIP file.")); return; }
  accountCommand({op:"drop",file:files[0]});
});
document.addEventListener("dragover", event => event.preventDefault());
document.addEventListener("drop", event => event.preventDefault());

document.getElementById("account-custom-select").addEventListener("click", () => accountCommand({op:"select-custom",replaceExisting:document.getElementById("account-custom-replace").checked}));

// 初期表示も同じ遷移経路を使い、非表示の入力欄へfocusを残さない。
tabMotion.snap("general");
navigationIndicator.schedule(false);
pageMotion.switchTo("home");


// 全画面共通の同期窓口。ファイル内容の走査は Native の変更通知後だけ行う。
let liveTimer = 0, liveRequest = 0, liveSerial = 0, liveSentAt = 0, liveFriendsKey = "";
let liveAppliedSerial = 0, liveFriendsSerial = 0;
const liveRevisions = new Map();
function synchronizeLauncherUi() {
  if (window.launcherCoreState?.corePhase !== "ready" || window.launcherCoreState?.stopRequested) return;
  if (liveRequest && performance.now() - liveSentAt < 3000) return;
  liveRequest = ++liveSerial; liveSentAt = performance.now();
  windowCommand(`ui:synchronize:${liveRequest}`);
}
window.addEventListener("launcher-live-state", event => {
  const requestId = event.detail?.requestId;
  if (!Number.isSafeInteger(requestId) || requestId <= 0 || requestId > liveSerial) return;
  if (requestId === liveRequest) liveRequest = 0;
  const state = event.detail.state;
  if (!state) return;
  // timeout 後の応答も受け取り、適用済みの新しい revision は上書きしない。
  if (requestId > liveAppliedSerial) {
    liveAppliedSerial = requestId;
    for (const name of ["graphicPacks","texture","world"]) {
      const revision = state[name];
      if (!Number.isSafeInteger(revision) || revision < 0 || liveRevisions.get(name) === revision) continue;
      liveRevisions.set(name,revision);
      window.dispatchEvent(new CustomEvent("launcher-library-change",{detail:{name,revision}}));
    }
  }
  const friends = state.friends;
  // Friends は差分だけ届くため、他の revision とは別に適用順を管理する。
  if (friends && requestId > liveFriendsSerial) {
    liveFriendsSerial = requestId;
    const key = `${friends.generation}:${friends.connectionId}:${friends.revision}`;
    if (key !== liveFriendsKey) {
      liveFriendsKey = key;
      window.dispatchEvent(new CustomEvent("launcher-friends-state",{detail:friends}));
    }
  }
});
window.addEventListener("launcher-core-state", () => {
  if (!liveTimer && window.launcherCoreState?.corePhase === "ready") {
    liveTimer = setInterval(synchronizeLauncherUi,250); synchronizeLauncherUi();
  }
});
document.addEventListener("visibilitychange", () => { if (!document.hidden) synchronizeLauncherUi(); });
window.addEventListener("pagehide", () => { clearInterval(liveTimer); liveTimer = liveRequest = 0; });

window.addEventListener("launcher-language-changed", () => {
  if (window.launcherCoreState) updateNativeSettings(window.launcherCoreState);
  renderAccount();
  navigationIndicator.schedule(false); tabIndicator.schedule(false);
  windowCommand("language:exit-labels:" + JSON.stringify([
    LauncherI18n.t("Exit Minecraft?"),
    LauncherI18n.t("Unsaved changes may be lost.") + "\n" + LauncherI18n.t("Use “Exit and save” in Minecraft first."),
    LauncherI18n.t("Cancel"), LauncherI18n.t("Exit game")
  ]));
});
