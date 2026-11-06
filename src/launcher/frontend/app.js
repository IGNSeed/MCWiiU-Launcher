"use strict";

const pages = ["home", "settings"];
const navigation = document.querySelectorAll("[data-page]");
navigation.forEach((button) => button.addEventListener("click", () => {
  const selected = button.dataset.page;
  if (!pages.includes(selected)) return;
  pages.forEach((page) => { document.getElementById(page).hidden = page !== selected; });
  navigation.forEach((item) => {
    if (item === button) item.setAttribute("aria-current", "page");
    else item.removeAttribute("aria-current");
  });
  document.getElementById("page-label").textContent = button.textContent;
  windowCommand("page:" + selected);
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
const coreLabels = {
  idle: ["準備中", "ゲームを準備しています", "起動状態を確認しています。"],
  validating: ["準備中", "ゲームを準備しています", "ゲームデータと設定を確認しています。"],
  initializing: ["初期化中", "ゲームを準備しています", "起動の準備を進めています。"],
  ready: ["準備完了", "Minecraft を起動できます", "保存済みのゲーム設定を使用します。"],
  preparing: ["起動中", "Minecraft を起動しています", "ゲームの読み込みを準備しています。"],
  vulkan: ["描画準備中", "Minecraft を起動しています", "Vulkan の描画を準備しています。"],
  running: ["プレイ中", "Minecraft をプレイ中", "Home でゲームに戻れます。"],
  stopping: ["終了中", "ゲームを終了しています", "終了処理が完了するとランチャーを閉じます。"],
  stopped: ["終了", "ゲームを終了しました", "ランチャーを閉じています。"],
  failed: ["起動失敗", "ゲームを起動できませんでした", "ゲームデータと保存済みの設定を確認してください。"],
  unavailable: ["起動機能なし", "この画面ではゲームを起動できません", "ランチャーの表示設定を変更できます。"],
};
window.addEventListener("launcher-core-state", (event) => {
  const state = event.detail;
  window.launcherCoreState = state;
  const phase = state.failed ? "failed" : state.stopRequested && !state.stopped ? "stopping" : state.phase;
  const [status, heading, description] = coreLabels[phase] || coreLabels.idle;
  document.querySelector(".status").textContent = status;
  play.disabled = state.canLaunch !== true;
  document.querySelector(".launch-panel h2").textContent = heading;
  document.querySelector(".launch-panel p").textContent = description;
  const closing = state.stopRequested === true || phase === "stopping" || phase === "stopped";
  navigation.forEach((button) => { button.disabled = closing; });
  scale.disabled = closing;
});
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
