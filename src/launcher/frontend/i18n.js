"use strict";

window.LauncherI18n = (() => {
  const languages = ["en", "ja", "ko", "zh-CN", "zh-TW", "es", "fr", "de"];
  let selected = "auto", systemLanguage = "en", current = "en", scheduled = false;
  const bindings = new WeakMap(), nodes = new Set();
  const collected = new FinalizationRegistry(reference => nodes.delete(reference));
  function normalize(value) { return value === "auto" || languages.includes(value) ? value : "en"; }
  function resolve(value) {
    if (typeof value !== "string") return "en";
    const tag = value.toLowerCase().replaceAll("_", "-");
    if (tag === "zh" || tag.startsWith("zh-")) return /(?:tw|hk|mo|hant)/.test(tag) ? "zh-TW" : "zh-CN";
    return languages.find(language => language.toLowerCase() === tag.split("-")[0]) || "en";
  }
  function t(key, values = []) {
    if (!key) return "";
    const message = window.LauncherLocales[current]?.[key] ?? key;
    // 置換値を再走査せず、ユーザー名・ファイル名などの内容をそのまま保持する。
    return message.replace(/\{(\d+)\}/g, (match, index) => index < values.length ? String(values[index]) : match);
  }
  function apply(node, property, value, attribute) {
    if (attribute) {
      if (node.getAttribute(property) !== value) node.setAttribute(property, value);
    } else if (node[property] !== value) node[property] = value;
  }
  function bind(node, property, render, attribute = false) {
    node.removeAttribute(property === "textContent" ? "data-i18n" : `data-i18n-${property}`);
    let values = bindings.get(node);
    if (!values) {
      values = new Map(); bindings.set(node, values);
      const reference = new WeakRef(node); nodes.add(reference); collected.register(node, reference);
    }
    values.set(property, {render, attribute});
    const value = render();
    apply(node, property, value, attribute);
    return value;
  }
  function refresh() {
    scheduled = false;
    document.documentElement.lang = current;
    for (const node of document.querySelectorAll("[data-i18n]")) node.textContent = t(node.dataset.i18n);
    for (const attr of ["aria-label", "title", "alt", "placeholder"]) {
      for (const node of document.querySelectorAll(`[data-i18n-${attr}]`)) node.setAttribute(attr, t(node.getAttribute(`data-i18n-${attr}`)));
    }
    for (const reference of nodes) {
      const node = reference.deref();
      if (!node || !node.isConnected) { nodes.delete(reference); if (node) bindings.delete(node); continue; }
      for (const [property, {render, attribute}] of bindings.get(node)) {
        const value = render();
        apply(node, property, value, attribute);
      }
    }
    window.dispatchEvent(new CustomEvent("launcher-language-changed", {detail:{language:current}}));
  }
  function setLanguage(value) {
    selected = normalize(value);
    const next = selected === "auto" ? resolve(systemLanguage) : selected;
    if (current === next) return;
    current = next;
    if (!scheduled) { scheduled = true; queueMicrotask(refresh); }
  }
  function setSystemLanguage(value) { systemLanguage = value; setLanguage(selected); }
  document.addEventListener("DOMContentLoaded", refresh, {once:true});
  return {t, bind, normalize, resolve, setLanguage, setSystemLanguage, get language() { return current; }};
})();
