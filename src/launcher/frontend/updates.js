"use strict";
(() => {
  const $ = id => document.getElementById(id);
  const banner = $("update-banner"), dialog = $("update-dialog");
  let state = {state:"Idle",revision:0}, dismissed = false, requested = "", approval = "";
  const messages = {
    Idle:"", get Checking() { return LauncherI18n.t("Checking for Updates"); }, get UpToDate() { return LauncherI18n.t("You're Up to Date"); }, get UpdateAvailable() { return LauncherI18n.t("Update Available"); },
    get Scheduled() { return LauncherI18n.t("Update scheduled — waiting for Minecraft and pending saves to finish."); },
    get Downloading() { return LauncherI18n.t("Downloading Update"); }, get Verifying() { return LauncherI18n.t("Verifying Update"); }, get Staging() { return LauncherI18n.t("Staging Update"); }, get Staged() { return LauncherI18n.t("Ready to Install"); },
    get AwaitingShutdown() { return LauncherI18n.t("Preparing to close the launcher and install the update"); }, get Failed() { return LauncherI18n.t("Unable to Check for Updates"); },
    get FoundationRequired() { return LauncherI18n.t("A newer launcher foundation is required. Download and install the latest setup from the official GitHub Releases page, then close the launcher normally before running setup."); }
  };
  window.addEventListener("launcher-language-changed", render);
  function hideBanner() { dismissed = true; banner.hidden = true; }
  function details() {
    if (!state.targetId) return;
    const foundation = state.state === "FoundationRequired";
    LauncherI18n.bind($("update-current"), "textContent", () => foundation ? LauncherI18n.t("Launcher {0} (retained)", [$("launcher-version").textContent]) : $("launcher-version").textContent);
    LauncherI18n.bind($("update-version"), "textContent", () => foundation ? LauncherI18n.t("Foundation {0} (launcher unchanged)", [state.foundationVersion]) : state.version);
    LauncherI18n.bind($("update-size"), "textContent", () => `${state.size.toLocaleString(LauncherI18n.language)} B`);
    LauncherI18n.bind($("update-notes"), "textContent", () => state.notes);
    if (!dialog.open) dialog.showModal();
    render();
  }
  function render() {
    const available = state.state === "UpdateAvailable";
    const active = ["Scheduled","Downloading","Verifying","Staging","Staged","AwaitingShutdown"].includes(state.state);
    const foundation = state.state === "FoundationRequired";
    const label = state.state === "Failed" && state.targetId ? LauncherI18n.t("Update Failed") : messages[state.state] ?? state.state;
    const text = label + (state.error ? ` (${state.error})` : "");
    LauncherI18n.bind($("update-status"), "textContent", () => text);
    LauncherI18n.bind($("update-progress-text"), "textContent", () => text);
    LauncherI18n.bind($("update-banner-text"), "textContent", () => foundation ? LauncherI18n.t("Foundation {0} or later is required. Install the latest setup manually.", [state.foundationVersion]) : active ? text : LauncherI18n.t("MCWiiU-Launcher v{0} is available.", [state.version]));
    banner.hidden = dismissed || (!available && !active && !foundation) || document.querySelector("main").dataset.view === "game";
    $("update-check").disabled = state.state === "Checking" || active;
    $("update-details").hidden = !state.targetId;
    $("update-now").disabled = !available && !foundation;
    LauncherI18n.bind($("update-now"), "textContent", () => foundation ? LauncherI18n.t("Open Official Releases") : LauncherI18n.t("Update Now"));
    $("update-cancel").disabled = state.state === "AwaitingShutdown";
    LauncherI18n.bind($("update-cancel"), "textContent", () => available ? LauncherI18n.t("Later") : state.canCancel ? LauncherI18n.t("Cancel Update") : LauncherI18n.t("Close"));
    const downloading = state.state === "Downloading";
    $("update-progress").hidden = !downloading;
    if (downloading) {
      $("update-progress").max = state.size;
      $("update-progress").value = state.downloaded;
      $("update-progress-text").textContent += ` — ${state.downloaded.toLocaleString()} / ${state.size.toLocaleString(LauncherI18n.language)} B (${Math.floor(100 * state.downloaded / state.size)}%)`;
    }
    updatePlay();
  }
  $("update-check").addEventListener("click", () => { windowCommand("update:check"); });
  $("update-view").addEventListener("click", details);
  $("update-details").addEventListener("click", details);
  $("update-later").addEventListener("click", hideBanner);
  $("update-close").addEventListener("click", hideBanner);
  $("update-now").addEventListener("click", () => {
    if (state.state === "FoundationRequired") { windowCommand("update:open-foundation-releases"); return; }
    if (state.state !== "UpdateAvailable" || approval) return;
    approval = state.targetId;
    $("update-now").disabled = true;
    drainSaves();
  });
  function cancel() {
    if (state.state === "AwaitingShutdown") return;
    approval = "";
    if (state.canCancel && state.state !== "UpdateAvailable") windowCommand("update:cancel");
    else hideBanner();
    dialog.close();
  }
  $("update-cancel").addEventListener("click", cancel);
  dialog.addEventListener("cancel", event => { event.preventDefault(); cancel(); });
  window.addEventListener("launcher-update-state", event => {
    const next = event.detail;
    if (!next || !Number.isSafeInteger(next.revision) || next.revision < state.revision) return;
    if (next.targetId !== state.targetId) { requested = ""; approval = ""; }
    state = next;
    window.launcherUpdateState = state;
    render();
    if (state.state === "Staged" && requested !== state.targetId && !settingsBusy() && !closeAfterSaves) {
      requested = state.targetId;
      windowCommand("update:install:" + state.targetId);
    }
  });
  window.addEventListener("launcher-core-state", () => {
    if (window.launcherCoreState?.phase === "running" && dialog.open) dialog.close();
    render();
  });
  setInterval(() => {
    if (approval && !settingsBusy()) {
      const target = approval; approval = "";
      if (state.state === "UpdateAvailable" && state.targetId === target) windowCommand("update:approve:" + target);
    }
    windowCommand("update:poll");
  }, 250);
  // 全 defer script の初期化完了後。Core とゲームデータの長い検証は待たない。
  setTimeout(() => windowCommand("update:ui-ready"), 0);
})();
