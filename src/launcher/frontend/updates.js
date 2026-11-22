"use strict";
(() => {
  const $ = id => document.getElementById(id);
  const banner = $("update-banner"), dialog = $("update-dialog");
  let state = {state:"Idle",revision:0}, dismissed = false, requested = "", approval = "";
  const messages = {
    Idle:"", Checking:"Checking for Updates", UpToDate:"You're Up to Date", UpdateAvailable:"Update Available",
    Scheduled:"Update scheduled — waiting for Minecraft and pending saves to finish.",
    Downloading:"Downloading Update", Verifying:"Verifying Update", Staging:"Staging Update", Staged:"Ready to Install",
    AwaitingShutdown:"Preparing to close the launcher and install the update", Failed:"Unable to Check for Updates",
    FoundationRequired:"A newer launcher foundation is required. Download and install the latest setup from the official GitHub Releases page, then close the launcher normally before running setup."
  };
  function hideBanner() { dismissed = true; banner.hidden = true; }
  function details() {
    if (!state.targetId) return;
    const foundation = state.state === "FoundationRequired";
    $("update-current").textContent = foundation ? `Launcher ${$("launcher-version").textContent} (retained)` : $("launcher-version").textContent;
    $("update-version").textContent = foundation ? `Foundation ${state.foundationVersion} (launcher unchanged)` : state.version;
    $("update-size").textContent = `${state.size.toLocaleString()} bytes`;
    $("update-notes").textContent = state.notes;
    if (!dialog.open) dialog.showModal();
    render();
  }
  function render() {
    const available = state.state === "UpdateAvailable";
    const active = ["Scheduled","Downloading","Verifying","Staging","Staged","AwaitingShutdown"].includes(state.state);
    const foundation = state.state === "FoundationRequired";
    const label = state.state === "Failed" && state.targetId ? "Update Failed" : messages[state.state] || state.state;
    const text = label + (state.error ? ` (${state.error})` : "");
    $("update-status").textContent = text;
    $("update-progress-text").textContent = text;
    $("update-banner-text").textContent = foundation ? `Foundation ${state.foundationVersion} or later is required. Install the latest setup manually.` : active ? text : `MCWiiU-Launcher v${state.version} is available.`;
    banner.hidden = dismissed || (!available && !active && !foundation) || document.querySelector("main").dataset.view === "game";
    $("update-check").disabled = state.state === "Checking" || active;
    $("update-details").hidden = !state.targetId;
    $("update-now").disabled = !available && !foundation;
    $("update-now").textContent = foundation ? "Open Official Releases" : "Update Now";
    $("update-cancel").disabled = state.state === "AwaitingShutdown";
    $("update-cancel").textContent = available ? "Later" : state.canCancel ? "Cancel Update" : "Close";
    const downloading = state.state === "Downloading";
    $("update-progress").hidden = !downloading;
    if (downloading) {
      $("update-progress").max = state.size;
      $("update-progress").value = state.downloaded;
      $("update-progress-text").textContent += ` — ${state.downloaded.toLocaleString()} / ${state.size.toLocaleString()} bytes (${Math.floor(100 * state.downloaded / state.size)}%)`;
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
