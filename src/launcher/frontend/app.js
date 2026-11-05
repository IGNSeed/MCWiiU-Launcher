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
