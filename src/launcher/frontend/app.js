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
const allowedScales = ["1", "1.1"];
function applyScale(value) {
  const accepted = allowedScales.includes(value) ? value : "1";
  scale.value = accepted;
  document.documentElement.style.setProperty("--scale", accepted);
}
try { applyScale(localStorage.getItem("interface-scale")); }
catch { applyScale("1"); }
scale.addEventListener("change", () => {
  applyScale(scale.value);
  try { localStorage.setItem("interface-scale", scale.value); }
  catch { document.getElementById("settings-note").textContent = "表示サイズは変更しました。保存領域が利用できないため、今回は保存されません。"; }
});
