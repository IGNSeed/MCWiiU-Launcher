"use strict";

const worldList = document.getElementById("world-list"), worldDialog = document.getElementById("world-dialog");
const worldName = document.getElementById("world-name"), worldConfirm = document.getElementById("world-confirm"), worldDestination = document.getElementById("world-destination");
let worldSnapshot = null, worldSerial = Date.now(), worldPending = 0, worldPendingOp = "", worldTarget = null, worldFocus = null, worldDirty = false, worldVisible = false, worldView = "worlds";
const worldImportDialog = document.getElementById("world-import-dialog"), worldImportConfirm = document.getElementById("world-import-confirm");
let worldImportTarget = null, worldImportDesiredName = "", worldImportNameDirty = false, worldImportNameTimer = 0;
let worldFolderCooldown = 0;
let worldImportNameTouched = false;
let worldRendered = "";
const worldImportName = document.getElementById("world-import-name");
const worldNotices = {
  "folder-opened":"", "folder-open-busy":"", get "world-folder-missing"() { return LauncherI18n.t("World save folder not found."); }, get "open-folder-failed"() { return LauncherI18n.t("Could not open File Explorer."); },
  ready:"", get saved() { return LauncherI18n.t("Saved."); }, get unchanged() { return LauncherI18n.t("No changes."); }, get cancelled() { return LauncherI18n.t("Operation cancelled."); }, get "preview-ready"() { return LauncherI18n.t("Review the preview, then save."); },
  get deleted() { return LauncherI18n.t("Moved to Trash."); }, get restored() { return LauncherI18n.t("Restored to the original account."); }, get imported() { return LauncherI18n.t("World imported."); }, get "import-preview-ready"() { return LauncherI18n.t("Review the world and destination before importing."); },
  get "installation-region-required"() { return LauncherI18n.t("Install BASE in Game Data to view saves for that region."); },
  get "not-ready"() { return LauncherI18n.t("Wait for the game to stop and Core to become ready."); }, get "stale-request"() { return LauncherI18n.t("Account or list changed. Retry after the list refreshes."); },
  get "stale-world"() { return LauncherI18n.t("World changed. Retry after the list refreshes."); }, get "invalid-name"() { return LauncherI18n.t("Enter a nonblank name of 1–32 UTF-16 units without control characters."); },
  get "save-id-conflict"() { return LauncherI18n.t("Save ID already exists at the destination. Nothing was overwritten."); }, get "invalid-image"() { return LauncherI18n.t("Cannot read image. Select a PNG / JPEG up to 16 MB."); },
  get "unsupported-metadata"() { return LauncherI18n.t("This save format cannot be edited safely."); }, get "invalid-world"() { return LauncherI18n.t("Cannot read this world safely."); }, get "invalid-account"() { return LauncherI18n.t("Cannot verify the account."); },
  get "unsafe-path"() { return LauncherI18n.t("Cannot verify a safe destination."); }, get "stale-preview"() { return LauncherI18n.t("Image preview expired. Select the image again."); },
  get "recovery-required"() { return LauncherI18n.t("Interrupted operation. Game launch and editing are blocked until recovery completes."); },
  get recovered() { return LauncherI18n.t("Restored the original world."); }, get "rolled-back"() { return LauncherI18n.t("Operation failed. Restored the original state."); }, get "read-failed"() { return LauncherI18n.t("Cannot read files. Close other applications using them."); },
  get "write-failed"() { return LauncherI18n.t("Write failed. Check free space and file access."); }, get "verification-failed"() { return LauncherI18n.t("Could not verify data after writing."); },
  get "invalid-file"() { return LauncherI18n.t("Cannot verify file format or size."); }, get "world-limit"() { return LauncherI18n.t("World count exceeds the limit."); }, get "invalid-request"() { return LauncherI18n.t("Cannot verify the operation. Wait for the list to refresh."); },
  get "insufficient-space"() { return LauncherI18n.t("Not enough free space at the destination."); }, get "invalid-trash"() { return LauncherI18n.t("Cannot verify stored data. Files are preserved."); }, get "stale-trash"() { return LauncherI18n.t("Trash changed. Wait for the list to refresh."); },
  get "trash-unavailable"() { return LauncherI18n.t("Stored data has already been restored or its operation is incomplete."); }, get "trash-count-limit"() { return LauncherI18n.t("Trash list exceeds the limit. No data will be deleted."); },
  get "stale-import-preview"() { return LauncherI18n.t("Preview expired. Select the import source again."); }, get "stale-import-source"() { return LauncherI18n.t("Source files changed. Select the import source again."); },
  get "invalid-body"() { return LauncherI18n.t("Invalid compressed world data or file index."); }, get "unsupported-body-format"() { return LauncherI18n.t("Unverified world format. No conversion is performed."); },
  get "body-size-limit"() { return LauncherI18n.t("Decompressed world size exceeds the limit."); }, get "unsafe-source-path"() { return LauncherI18n.t("Cannot verify a safe import source. Links are unsupported."); },
  get "invalid-save-id"() { return LauncherI18n.t("Select an extensionless world file or .ext with a 12-digit Save ID."); }, get "missing-pair"() { return LauncherI18n.t("Selected file not found."); },
  get "unsupported-title"() { return LauncherI18n.t("Title folders from other regions are unsupported."); }, get "unsupported-import-format"() { return LauncherI18n.t("Select an extensionless world file with a 12-digit Save ID. Folders, ZIP and .wii are unsupported."); },
  get "unsupported-metadata-file"() { return LauncherI18n.t("Select an .ext metadata file with a 12-digit Save ID."); }, get "metadata-id-mismatch"() { return LauncherI18n.t("Metadata Save ID does not match the world file."); },
  get "world-file-required"() { return LauncherI18n.t("Select a world file."); }, get "import-icon-required"() { return LauncherI18n.t("Importing without metadata requires a PNG / JPEG icon."); },
  get "invalid-import-preview"() { return LauncherI18n.t("Review the name, image and file preview."); }, get "invalid-import-mode"() { return LauncherI18n.t("Import mode changed. Review your selection."); },
  get "invalid-import-source"() { return LauncherI18n.t("Cannot verify import source."); }, get "trash-empty"() { return LauncherI18n.t("Trash is empty."); }, get "purged"() { return LauncherI18n.t("Selected stored data permanently deleted."); },
  get "purge-preview-ready"() { return LauncherI18n.t("Review the selected data and size. Permanent deletion cannot be undone."); }, get "purge-blocked"() { return LauncherI18n.t("Deletion blocked by unsafe items. No deletion was started."); },
  get "purge-incomplete"() { return LauncherI18n.t("Stored item has an incomplete transaction."); }, get "purge-owner-mismatch"() { return LauncherI18n.t("Cannot match the original transaction."); }, get "purge-unknown-file"() { return LauncherI18n.t("Some files have unknown ownership."); },
  get "unsafe-purge-file"() { return LauncherI18n.t("Some files cannot be safely deleted, including links or read-only files."); }, get "purge-file-busy"() { return LauncherI18n.t("File in use. Cannot delete."); },
  get "purge-changed"() { return LauncherI18n.t("Files or Trash changed after confirmation. Retry after the list refreshes."); }, get "purge-write-failed"() { return LauncherI18n.t("Permanent deletion interrupted. Explicit recovery is required to continue."); },
  get "purge-backup-referenced"() { return LauncherI18n.t("Related backups are preserved because another transaction may reference them."); },
  get "purge-backup-protected"() { return LauncherI18n.t("Related backups cannot be safely cleaned up and are preserved."); }, get "purge-backup-limit"() { return LauncherI18n.t("Backup count exceeds the limit."); },
  get "purge-protected"() { return LauncherI18n.t("Cannot safely delete stored data. Files are preserved."); }, get "invalid-purge"() { return LauncherI18n.t("Cannot verify permanent deletion transaction."); },
  get "stale-purge-preview"() { return LauncherI18n.t("Deletion confirmation expired. Review the targets again."); },
  get "purge-plan-limit"() { return LauncherI18n.t("Deletion plan exceeds the size limit. Delete items individually."); }
};
function worldMessage(notice) { return worldNotices[notice] ?? LauncherI18n.t("Could not complete the operation."); }
function worldNode(tag, text, className) {
  const node = document.createElement(tag);
  if (text !== undefined) LauncherI18n.bind(node, "textContent", () => (text));
  if (className) node.className = className;
  return node;
}
function validWorldName(name) {
  return name.length > 0 && name.length <= 32 && name.isWellFormed() && !/[\u0000-\u001f\u007f]/u.test(name) && !/^[ \u3000]*$/u.test(name);
}
function worldCanEdit() { return worldSnapshot?.canEdit === true && window.launcherCoreState?.canEditWorlds === true && !worldPending; }
function worldCanManageAll() { return worldCanEdit() && window.launcherCoreState?.canRecoverWorlds === true; }
function worldCanOperate(op) { return ["rename","delete"].includes(op) ? worldCanEdit() : worldCanManageAll(); }
function worldPayload(op, target = worldTarget, extra = {}) {
  return {op,activeId:target?.activeId ?? worldSnapshot.activeId,revision:target?.revision ?? worldSnapshot.revision,
    ...(target?.id ? {world:target.id,version:target.version} : {}),...extra};
}
function sendWorld(op, payload, picker = false, importPart = null) {
  if (worldPending) return;
  worldPending = ++worldSerial; worldPendingOp = op;
  LauncherI18n.bind(document.getElementById("world-notice"), "textContent", () => (op === "list" ? LauncherI18n.t("Loading worlds…") : LauncherI18n.t("Working…")));
  LauncherI18n.bind(document.getElementById("world-dialog-notice"), "textContent", () => (op === "list" ? "" : LauncherI18n.t("Working…")));
  if (worldImportDialog.open) showWorldImportNotice("");
  updateWorldControls();
  windowCommand(`world:${op === "open-folder" ? "open-folder" : importPart !== null ? `select-import:${importPart}` : picker ? "select-image" : "command"}:${worldPending}:${JSON.stringify(payload)}`);
}
function updateWorldControls() {
  const enabled = worldCanEdit();
  document.getElementById("world-import").disabled = !enabled;
  document.getElementById("world-open-folder").disabled = !enabled || performance.now() < worldFolderCooldown;
  document.getElementById("world-trash").disabled = !!worldPending || window.launcherCoreState?.corePhase !== "ready";
  document.getElementById("world-back").disabled = !!worldPending;
  worldList.setAttribute("aria-busy", String(!!worldPending));
  for (const button of worldList.querySelectorAll("button")) button.disabled = !worldCanOperate(button.dataset.worldOperation) || button.dataset.purgeBlocked === "true";
  document.getElementById("world-empty-trash").disabled = !worldCanManageAll() || !worldSnapshot?.trash.length;
  const current = worldTarget && worldSnapshot && worldTarget.activeId === worldSnapshot.activeId && worldTarget.revision === worldSnapshot.revision;
  worldName.disabled = !enabled;
  worldDestination.disabled = document.getElementById("world-select-image").disabled = !worldCanManageAll();
  const recover = worldTarget?.op === "recover";
  worldConfirm.disabled = !current || (recover ? !!worldPending || window.launcherCoreState?.canRecoverWorlds !== true : !worldCanOperate(worldTarget?.op)) ||
    (worldTarget?.op === "rename" && !validWorldName(worldName.value)) || (worldTarget?.op === "icon" && worldSnapshot?.preview?.world !== worldTarget.id) ||
    (worldTarget?.op === "move" && !worldSnapshot.accounts.some(account => account.id === Number(worldDestination.value))) ||
    (worldTarget?.op === "restore" && (!worldTarget.accountAvailable || worldTarget.problem || worldTarget.status !== "deleted")) ||
    (["purge","empty-trash"].includes(worldTarget?.op) && (!worldSnapshot?.purgePreview || worldSnapshot.purgeIssues.length > 0));
  document.getElementById("world-cancel").disabled = !!worldPending && worldPendingOp !== "list";
  document.getElementById("world-form").setAttribute("aria-busy", String(!!worldPending));
  document.getElementById("world-recover").disabled = !!worldPending || window.launcherCoreState?.canRecoverWorlds !== true;
  const importCurrent = worldImportTarget && worldSnapshot && worldImportTarget.activeId === worldSnapshot.activeId && worldImportTarget.revision === worldSnapshot.revision;
  const candidate = worldSnapshot?.importPreview?.candidate;
  const baseReady = worldSnapshot?.canEdit === true && window.launcherCoreState?.canEditWorlds === true && importCurrent;
  const nameCurrent = !candidate?.generated || (!worldImportNameDirty && validWorldName(worldImportName.value) && worldImportName.value === candidate.name);
  worldImportConfirm.disabled = !enabled || !importCurrent || !candidate?.ready || !worldSnapshot.importPreview.token || !nameCurrent;
  for (const id of ["world-import-body","world-import-metadata"]) document.getElementById(id).disabled = !enabled || !importCurrent;
  document.getElementById("world-import-clear-metadata").disabled = !enabled || !importCurrent || !candidate?.metadataFile;
  document.getElementById("world-import-image").disabled = !enabled || !importCurrent || !candidate?.worldFile || !candidate.generated;
  worldImportName.disabled = !baseReady || !candidate?.worldFile || !candidate.generated || (!!worldPending && worldPendingOp !== "name-import");
  document.getElementById("world-import-cancel").disabled = !!worldPending;
  document.getElementById("world-import-form").setAttribute("aria-busy", String(!!worldPending));
}
function renderWorlds() {
  const state = worldSnapshot;
  const key = LauncherI18n.language + JSON.stringify([state?.activeId,state?.worlds,state?.trash,state?.trashBytes,worldView,state?.recoveryRequired,state?.recoveryMode,state?.purgeProgress,state?.purgeIssues]);
  if (key === worldRendered) { updateWorldControls(); return; }
  worldRendered = key;
  const active = accountSnapshot?.accounts?.find(account => account.id === state?.activeId);
  LauncherI18n.bind(document.getElementById("world-account"), "textContent", () => (state ? `${active?.name || LauncherI18n.t("Active Account")} · ${state.activeId.toString(16).padStart(8,"0")}` : ""));
  document.getElementById("world-recover").hidden = !state?.recoveryRequired;
  LauncherI18n.bind(document.getElementById("world-title"), "textContent", () => (worldView === "trash" ? LauncherI18n.t("Recently Deleted") : "World"));
  document.getElementById("world-open-folder").hidden = worldView === "trash";
  document.getElementById("world-import").hidden = document.getElementById("world-trash").hidden = worldView === "trash";
  document.getElementById("world-back").hidden = document.getElementById("world-empty-trash").hidden = worldView !== "trash";
  document.getElementById("world-trash-capacity").hidden = worldView !== "trash";
  LauncherI18n.bind(document.getElementById("world-trash-capacity"), "textContent", () => (LauncherI18n.t("Stored: {0} worlds · Verified storage (including restored items): {1} MiB", [state?.trash.length || 0, new Intl.NumberFormat("ja-JP",{maximumFractionDigits:2}).format((state?.trashBytes || 0) / 1048576)])));
  const rows = document.createDocumentFragment();
  for (const world of (worldView === "trash" ? state?.trash : state?.worlds) || []) {
    const card = worldNode("article", undefined, "world-card");
    if (world.icon) { const image = document.createElement("img"); image.src = world.icon; LauncherI18n.bind(image, "alt", () => ("")); image.width = image.height = 64; card.append(image); }
    else card.append(worldNode("div", "?", "world-icon-empty"));
    const info = worldNode("div", undefined, "world-info");
    info.append(worldNode("h2",world.name || LauncherI18n.t("Unreadable world · {0}", [world.id])));
    info.append(worldNode("p",world.problem ? LauncherI18n.t("Files are preserved. Editing is unavailable.") : `${new Intl.NumberFormat("ja-JP",{maximumFractionDigits:2}).format(world.size / 1048576)} MB`,"footnote"));
    if (worldView === "trash" && !world.problem) {
      info.append(worldNode("p",`Original Account · ${world.account.toString(16).padStart(8,"0")} · Save ID ${world.id}`,"footnote"));
      if (!world.accountAvailable) info.append(worldNode("p",LauncherI18n.t("Cannot restore because the original account is not registered."),"footnote"));
      info.append(worldNode("p",`Deleted · ${new Date(world.deletedAt).toLocaleString("ja-JP")}${world.status === "restored" ? LauncherI18n.t(" · Restored, retained") : world.status === "cancelled" ? LauncherI18n.t(" · Cancelled, retained") : world.status === "incomplete" ? LauncherI18n.t(" · Incomplete transaction") : ""}`,"footnote"));
    }
    card.append(info);
    if (worldView === "trash" && (world.problem || world.purgeProblem)) info.append(worldNode("p",LauncherI18n.t("Cannot permanently delete · {0}", [worldMessage(world.purgeProblem || "invalid-trash")]),"footnote"));
    if (!world.problem || worldView === "trash") {
      const actions = worldNode("div", undefined, "world-actions");
      const operations = worldView === "trash" ? [...(!world.problem && world.status === "deleted" && world.accountAvailable ? [["restore",LauncherI18n.t("Restore")]] : []),["purge",LauncherI18n.t("Delete")]] : [["rename",LauncherI18n.t("Rename")],["icon",LauncherI18n.t("Change Icon")],["move",LauncherI18n.t("Move")],["delete",LauncherI18n.t("Delete")]];
      for (const [op,label] of operations) {
        const button = worldNode("button",label); button.type = "button"; button.dataset.worldOperation = op; button.dataset.world = world.id;
        LauncherI18n.bind(button, "aria-label", () => (`${op === "purge" ? LauncherI18n.t("Delete Permanently") : label} · ${world.name}`), true); actions.append(button);
        if (["delete","purge"].includes(op)) button.classList.add("world-danger");
        if (op === "purge") button.dataset.purgeBlocked = String(!!world.problem || !!world.purgeProblem);
        if (worldView === "trash") button.dataset.trash = world.key;
      }
      card.append(actions);
    }
    rows.append(card);
  }
  if (!(worldView === "trash" ? state?.trash?.length : state?.worlds.length)) rows.append(worldNode("p",state ? worldView === "trash" ? LauncherI18n.t("No deleted worlds.") : LauncherI18n.t("No worlds in this account.") : LauncherI18n.t("Preparing…"),"friends-empty"));
  worldList.replaceChildren(rows);
  const progress = document.getElementById("world-purge-progress"), counts = state?.purgeProgress;
  progress.hidden = state?.recoveryMode !== "purge" || !state?.recoveryRequired;
  LauncherI18n.bind(progress, "textContent", () => (counts ? LauncherI18n.t("Deletion interrupted. Completed {0} / {1}, remaining {2} (partially deleted {3}), blocking issues {4}. Recovery continues deletion.", [counts.completed, counts.total, counts.remaining, counts.partial, counts.protected]) : ""));
  if (!progress.hidden && state.purgeIssues?.length) progress.textContent += " " + state.purgeIssues.map(issue => `${issue.name || issue.key || LauncherI18n.t("Target data")}: ${worldMessage(issue.reason)}`).join(" / ");
  updateWorldControls();
}
function openWorldDialog(op, world, opener = document.activeElement) {
  if (op === "recover" ? !worldSnapshot?.recoveryRequired || worldPending || window.launcherCoreState?.canRecoverWorlds !== true : !worldCanOperate(op)) return;
  worldTarget = {...world,op,activeId:worldSnapshot.activeId,revision:worldSnapshot.revision}; worldFocus = opener;
  LauncherI18n.bind(document.getElementById("world-dialog-title"), "textContent", () => ({get rename() { return LauncherI18n.t("Rename World"); },get icon() { return LauncherI18n.t("Change Icon"); },get move() { return LauncherI18n.t("Move World"); },get recover() { return LauncherI18n.t("World recovery"); },get delete() { return LauncherI18n.t("Delete World"); },get restore() { return LauncherI18n.t("Restore World"); },get purge() { return LauncherI18n.t("Delete Permanently"); }, get "empty-trash"() { return LauncherI18n.t("Empty Trash"); }}[op]));
  LauncherI18n.bind(document.getElementById("world-dialog-description"), "textContent", () => (op === "recover" ? LauncherI18n.t("Compare backups with current files and restore the previous state. Stop without changes if they do not match.") : op === "move" ? LauncherI18n.t("Move only this world to the selected account. Existing destination worlds are preserved.") : op === "delete" ? LauncherI18n.t("Remove this world from the game list. Its body and .ext are stored in Trash and can be restored to the original account.") : op === "restore" ? LauncherI18n.t("Restore to the original account. Existing worlds are not overwritten.") : ""));
  if (["purge","empty-trash"].includes(op)) LauncherI18n.bind(document.getElementById("world-dialog-description"), "textContent", () => ((op === "purge" ? LauncherI18n.t("Permanently delete this world? This cannot be undone.") : LauncherI18n.t("Permanently delete all worlds in Trash? This cannot be undone.")) + LauncherI18n.t("Deletes stored items and directly linked transaction copies. Restored worlds and independent backups are preserved.")));
  if (op === "recover" && worldSnapshot.recoveryMode === "purge") LauncherI18n.bind(document.getElementById("world-dialog-description"), "textContent", () => (LauncherI18n.t("Continue interrupted permanent deletion. Remaining files are checked against the saved plan; deleted data is not restored.")));
  document.getElementById("world-purge-issues").hidden = !["purge","empty-trash"].includes(op); document.getElementById("world-purge-issues").replaceChildren();
  LauncherI18n.bind(document.getElementById("world-dialog-target"), "textContent", () => (world?.name || ""));
  document.getElementById("world-name-row").hidden = op !== "rename";
  document.getElementById("world-icon-row").hidden = op !== "icon";
  document.getElementById("world-move-row").hidden = op !== "move";
  document.getElementById("world-confirm-details").hidden = !["delete","restore","purge"].includes(op);
  const confirmIcon = document.getElementById("world-confirm-icon"); confirmIcon.removeAttribute("src"); if (["delete","restore","purge"].includes(op) && world?.icon) confirmIcon.src = world.icon;
  const owner = ["restore","purge"].includes(op) ? world.account : worldSnapshot.activeId;
  const ownerName = accountSnapshot?.accounts?.find(account => account.id === owner)?.name || "Account";
  LauncherI18n.bind(document.getElementById("world-confirm-location"), "textContent", () => (world ? `Save ID · ${world.id}\n${ownerName} · ${owner.toString(16).padStart(8,"0")}${op === "purge" ? `\n${(world.size / 1048576).toFixed(2)} MiB` : ""}` : ""));
  worldName.value = world?.name || "";
  worldDestination.replaceChildren(...worldSnapshot.accounts.map(account => {
    const option = worldNode("option",`${account.name} · ${account.id.toString(16).padStart(8,"0")}`); option.value = String(account.id); return option;
  }));
  const image = document.getElementById("world-icon-preview"); image.removeAttribute("src"); if (op === "icon" && world?.icon) image.src = world.icon;
  LauncherI18n.bind(document.getElementById("world-dialog-notice"), "textContent", () => (op === "move" && !worldSnapshot.accounts.length ? LauncherI18n.t("No other registered accounts.") : ""));
  LauncherI18n.bind(worldConfirm, "textContent", () => (op === "move" ? LauncherI18n.t("Move to this account") : op === "recover" ? LauncherI18n.t("Restore previous state") : op === "delete" ? LauncherI18n.t("Delete World") : op === "restore" ? LauncherI18n.t("Restore World") : op === "purge" ? LauncherI18n.t("Delete Permanently") : op === "empty-trash" ? LauncherI18n.t("Empty Trash") : LauncherI18n.t("Save")));
  if (op === "recover" && worldSnapshot.recoveryMode === "purge") { LauncherI18n.bind(worldConfirm, "textContent", () => (LauncherI18n.t("Continue deletion"))); LauncherI18n.bind(document.getElementById("world-dialog-title"), "textContent", () => (LauncherI18n.t("Continue permanent deletion"))); }
  worldConfirm.classList.toggle("world-danger",["delete","purge","empty-trash"].includes(op));
  worldDialog.showModal(); updateWorldControls();
  (op === "rename" ? worldName : document.getElementById("world-cancel")).focus();
  if (["purge","empty-trash"].includes(op)) sendWorld("prepare-purge",{op:"prepare-purge",activeId:worldTarget.activeId,revision:worldTarget.revision,...(op === "purge" ? {trash:world.key,version:world.version} : {})});
}
function showWorldImportNotice(notice, op = "") {
  const fields = {body:"world-import-body",metadata:"world-import-metadata",name:"world-import-name",icon:"world-import-image"};
  const messages = {};
  const quiet = ["","ready","cancelled","world-file-required","import-icon-required","import-preview-ready","invalid-name"];
  if (!quiet.includes(notice)) {
    const part = notice === "metadata-id-mismatch" ? "metadata" : op.startsWith("select-import-") ? op.slice(14) : "";
    if (fields[part]) messages[part] = notice === "metadata-id-mismatch" ? LauncherI18n.t("Save ID does not match the world file.") : notice === "invalid-image" ? LauncherI18n.t("Select a PNG / JPEG image.") : worldMessage(notice);
    else messages.general = worldMessage(notice);
  }
  if (worldImportNameTouched && !document.getElementById("world-import-generated").hidden && !validWorldName(worldImportName.value)) messages.name = LauncherI18n.t("Enter a valid name of 1–32 UTF-16 units.");
  for (const [part,id] of Object.entries(fields)) {
    const error = document.getElementById(`world-import-${part}-error`);
    LauncherI18n.bind(error, "textContent", () => (messages[part] || "")); error.hidden = !messages[part];
    document.getElementById(id).setAttribute("aria-invalid",String(!!messages[part]));
  }
  const general = document.getElementById("world-import-notice"); LauncherI18n.bind(general, "textContent", () => (messages.general || "")); general.hidden = !messages.general;
}
function renderWorldImport() {
  if (!worldImportDialog.open) return;
  const candidate = worldSnapshot?.importPreview?.candidate;
  for (const [id,value,fallback] of [["world-import-body-file",candidate?.worldFile,LauncherI18n.t("No file selected")],["world-import-metadata-file",candidate?.metadataFile,LauncherI18n.t("Not selected")],["world-import-image-file",candidate?.imageFile,"PNG / JPEG"]]) {
    const field = document.getElementById(id); LauncherI18n.bind(field, "textContent", () => (value || fallback)); LauncherI18n.bind(field, "title", () => (value || ""));
  }
  document.getElementById("world-import-clear-metadata").hidden = !candidate?.metadataFile;
  document.getElementById("world-import-generated").hidden = !candidate?.worldFile || candidate.generated === false;
  const image = document.getElementById("world-import-icon-image"), hasImage = candidate?.generated && !!candidate.icon;
  image.hidden = !hasImage; if (hasImage) image.src = candidate.icon; else image.removeAttribute("src");
  document.getElementById("world-import-icon-empty").hidden = hasImage;
  if (!worldImportNameDirty && document.activeElement !== worldImportName) worldImportName.value = candidate?.generated ? worldImportDesiredName : "";
  const ready = candidate?.ready && !worldImportNameDirty;
  const row = worldNode("div",undefined,"world-import-candidate");
  if (ready) {
    const icon = document.createElement("img"); icon.src = candidate.icon; LauncherI18n.bind(icon, "alt", () => (LauncherI18n.t("World Icon"))); row.append(icon);
    const info = worldNode("div"); info.append(worldNode("strong",candidate.name));
    info.append(worldNode("p",`Save ID · ${candidate.id} · ${(candidate.size / 1048576).toFixed(2)} MiB`)); row.append(info);
  }
  document.getElementById("world-import-preview").replaceChildren(ready ? row : worldNode("p",candidate?.worldFile ? (candidate.generated ? LauncherI18n.t("Choose a name and icon.") : LauncherI18n.t("Check the metadata file.")) : LauncherI18n.t("Select a file to preview it."),"world-import-preview-empty"));
  document.getElementById("world-import-account-hint").hidden = !ready || candidate.account === worldImportTarget?.activeId.toString(16).padStart(8,"0");
  updateWorldControls();
}
function flushImportName() {
  clearTimeout(worldImportNameTimer);
  if (!worldImportDialog.open || !worldImportTarget || !worldImportNameDirty || worldPending || worldSnapshot?.importPreview?.candidate.generated === false) return;
  const name = validWorldName(worldImportDesiredName) ? worldImportDesiredName : "";
  sendWorld("name-import",{op:"name-import",activeId:worldImportTarget.activeId,revision:worldImportTarget.revision,name});
}
function selectWorldImport(part) {
  if (worldCanEdit() && worldImportTarget) sendWorld(`select-import-${part}`,{op:"select-import",activeId:worldImportTarget.activeId,revision:worldImportTarget.revision},false,part);
}
document.getElementById("world-import").addEventListener("click", () => {
  if (!worldCanEdit()) return;
  worldImportTarget = {activeId:worldSnapshot.activeId,revision:worldSnapshot.revision}; worldImportDesiredName = ""; worldImportNameDirty = false; worldImportName.value = ""; worldImportNameTouched = false;
  const name = accountSnapshot?.accounts?.find(account => account.id === worldSnapshot.activeId)?.name || LauncherI18n.t("Active Account");
  LauncherI18n.bind(document.getElementById("world-import-destination"), "textContent", () => (`Import to · ${name} · ${worldSnapshot.activeId.toString(16).padStart(8,"0")}`));
  showWorldImportNotice("");
  document.getElementById("world-import-preview").replaceChildren(); worldImportDialog.showModal(); worldImportDialog.scrollTop = 0; worldImportDialog.querySelector(".world-import-content").scrollTop = 0;
  sendWorld("begin-import",{op:"begin-import",activeId:worldImportTarget.activeId,revision:worldImportTarget.revision});
});
for (const [id,part] of [["world-import-body","body"],["world-import-metadata","metadata"],["world-import-image","icon"]]) document.getElementById(id).addEventListener("click",() => selectWorldImport(part));
document.getElementById("world-import-clear-metadata").addEventListener("click",() => {
  if (worldCanEdit() && worldImportTarget) sendWorld("clear-import-metadata",{op:"clear-import-metadata",activeId:worldImportTarget.activeId,revision:worldImportTarget.revision});
});
worldImportName.addEventListener("input",() => {
  worldImportDesiredName = worldImportName.value; worldImportNameDirty = true; worldImportNameTouched = true; clearTimeout(worldImportNameTimer); updateWorldControls();
  renderWorldImport(); showWorldImportNotice("");
  worldImportNameTimer = setTimeout(flushImportName,200);
});
document.getElementById("world-import-cancel").addEventListener("click", () => { if (!worldPending) worldImportDialog.close(); });
worldImportDialog.addEventListener("cancel", event => { if (worldPending) event.preventDefault(); });
worldImportDialog.addEventListener("close", () => {
  clearTimeout(worldImportNameTimer); worldImportTarget = null; worldImportDesiredName = ""; worldImportNameDirty = false;
  if (selectedPage === "world") document.getElementById("world-import").focus({preventScroll:true});
  if (worldSnapshot?.importPreview && !worldPending) sendWorld("clear-import",{op:"clear-import",activeId:worldSnapshot.activeId,revision:worldSnapshot.revision});
});
worldImportDialog.addEventListener("keydown", event => {
  if (event.key !== "Tab") return;
  const controls = [...worldImportDialog.querySelectorAll("input,button")].filter(node => !node.disabled && node.getClientRects().length);
  if (event.shiftKey && document.activeElement === controls[0]) { event.preventDefault(); controls.at(-1)?.focus(); }
  else if (!event.shiftKey && document.activeElement === controls.at(-1)) { event.preventDefault(); controls[0]?.focus(); }
});
document.getElementById("world-import-form").addEventListener("submit", event => {
  event.preventDefault(); if (worldImportConfirm.disabled || !worldImportTarget || !worldSnapshot?.importPreview) return;
  sendWorld("import",{op:"import",activeId:worldImportTarget.activeId,revision:worldImportTarget.revision,token:worldSnapshot.importPreview.token,mode:worldSnapshot.importPreview.candidate.generated ? "generated" : "metadata",...(worldSnapshot.importPreview.candidate.generated ? {name:worldImportName.value} : {})});
});
function updateWorldView() {
  const visible = selectedPage === "world" && !document.hidden;
  if (!visible) { worldVisible = false; if (!worldPending) { if (worldDialog.open) worldDialog.close(); if (worldImportDialog.open) worldImportDialog.close(); } return; }
  updateWorldControls();
  if (window.launcherCoreState?.phase === "unavailable") { LauncherI18n.bind(document.getElementById("world-notice"), "textContent", () => (LauncherI18n.t("Connect to Core to view worlds."))); return; }
  if ((!worldVisible || worldDirty) && !worldPending && !worldDialog.open && !worldImportDialog.open && (window.launcherCoreState?.canEditWorlds === true || window.launcherCoreState?.canRecoverWorlds === true)) { worldVisible = true; worldDirty = false; sendWorld(worldView === "trash" ? "trash-list" : "list",{op:worldView === "trash" ? "trash-list" : "list"}); }
}
window.updateWorldView = updateWorldView;
worldList.addEventListener("click", event => {
  const button = event.target.closest("[data-world-operation]"); if (!button || button.disabled) return;
  const world = button.dataset.trash ? worldSnapshot?.trash.find(world => world.key === button.dataset.trash) : worldSnapshot?.worlds.find(world => world.id === button.dataset.world); if (world) openWorldDialog(button.dataset.worldOperation,world,button);
});
document.getElementById("world-open-folder").addEventListener("click", () => {
  if (!worldCanEdit() || performance.now() < worldFolderCooldown) return;
  worldFolderCooldown = performance.now() + 1000;
  sendWorld("open-folder",{op:"open-folder",activeId:worldSnapshot.activeId,revision:worldSnapshot.revision});
  setTimeout(updateWorldControls,1000);
});
document.getElementById("world-trash").addEventListener("click", () => { worldView = "trash"; renderWorlds(); sendWorld("trash-list",{op:"trash-list"}); document.getElementById("world-back").focus(); });
document.getElementById("world-back").addEventListener("click", () => { worldView = "worlds"; renderWorlds(); sendWorld("list",{op:"list"}); document.getElementById("world-trash").focus(); });
document.getElementById("world-empty-trash").addEventListener("click", () => openWorldDialog("empty-trash"));
document.getElementById("world-recover").addEventListener("click", () => openWorldDialog("recover"));
document.getElementById("world-select-image").addEventListener("click", () => { if (worldCanManageAll()) sendWorld("preview",worldPayload("preview"),true); });
document.getElementById("world-cancel").addEventListener("click", () => { if (!worldPending) worldDialog.close(); });
worldDialog.addEventListener("cancel", event => { if (worldPending) event.preventDefault(); });
worldDialog.addEventListener("close", () => {
  const target = worldTarget; worldTarget = null;
  if (selectedPage === "world") (worldFocus?.isConnected && !worldFocus.disabled ? worldFocus : document.getElementById("world-open-folder")).focus({preventScroll:true});
  if (["purge","empty-trash"].includes(target?.op) && !worldPending && worldSnapshot?.purgePreview) sendWorld("clear-purge",{op:"clear-purge",activeId:worldSnapshot.activeId,revision:worldSnapshot.revision});
  if (target?.op === "icon" && worldSnapshot?.preview && !worldPending) sendWorld("clear-preview",worldPayload("clear-preview",target));
});
worldName.addEventListener("input", () => { LauncherI18n.bind(document.getElementById("world-dialog-notice"), "textContent", () => (validWorldName(worldName.value) ? "" : worldNotices["invalid-name"])); updateWorldControls(); });
worldDestination.addEventListener("change", updateWorldControls);
document.getElementById("world-form").addEventListener("submit", event => {
  event.preventDefault(); if (worldConfirm.disabled || !worldTarget) return;
  const op = worldTarget.op;
  const extra = op === "rename" ? {name:worldName.value} : op === "icon" ? {token:worldSnapshot.preview.token} : op === "move" ? {destination:Number(worldDestination.value)} : {};
  if (["purge","empty-trash"].includes(op)) { sendWorld("purge",{op:"purge",activeId:worldTarget.activeId,revision:worldTarget.revision,token:worldSnapshot.purgePreview.token}); return; }
  sendWorld(op,op === "restore" ? {op,activeId:worldTarget.activeId,revision:worldTarget.revision,trash:worldTarget.key,version:worldTarget.version} : worldPayload(op,worldTarget,extra));
});
worldDialog.addEventListener("keydown", event => {
  if (event.key === "Enter" && ["purge","empty-trash"].includes(worldTarget?.op)) { event.preventDefault(); return; }
  if (event.key !== "Tab") return;
  const controls = [...worldDialog.querySelectorAll("input,select,button")].filter(node => !node.disabled && node.getClientRects().length);
  if (event.shiftKey && document.activeElement === controls[0]) { event.preventDefault(); controls.at(-1)?.focus(); }
  else if (!event.shiftKey && document.activeElement === controls.at(-1)) { event.preventDefault(); controls[0]?.focus(); }
});
window.addEventListener("launcher-world-result", event => {
  if (event.detail?.requestId !== worldPending) return;
  const state = event.detail.world, op = worldPendingOp; worldPending = 0; worldPendingOp = "";
  if (!state || !Number.isSafeInteger(state.revision) || !Number.isInteger(state.activeId) || !Array.isArray(state.worlds) || !Array.isArray(state.accounts) || state.worlds.length > 1000 ||
      state.worlds.some(world => !/^[0-9]{12}$/.test(world.id) || typeof world.name !== "string" || typeof world.version !== "string" || (world.icon && !/^data:image\/png;base64,[A-Za-z0-9+/=]+$/.test(world.icon))) ||
      !Array.isArray(state.trash) || state.trash.length > 4096 || state.trash.some(entry => typeof entry.key !== "string" || typeof entry.name !== "string" || (entry.icon && !/^data:image\/png;base64,[A-Za-z0-9+/=]+$/.test(entry.icon))) ||
      (state.importPreview && (typeof state.importPreview.token !== "string" || !state.importPreview.candidate || (state.importPreview.candidate.id && !/^[0-9]{12}$/.test(state.importPreview.candidate.id)) || typeof state.importPreview.candidate.name !== "string" || (state.importPreview.candidate.icon && !/^data:image\/png;base64,[A-Za-z0-9+/=]+$/.test(state.importPreview.candidate.icon))))) {
    LauncherI18n.bind(document.getElementById("world-notice"), "textContent", () => (worldMessage(state?.notice))); updateWorldControls(); return;
  }
  worldSnapshot = state;
  if (worldImportDialog.open) {
    if (["begin-import","clear-import-metadata","select-import-body","select-import-metadata"].includes(op) && state.notice !== "cancelled") {
      clearTimeout(worldImportNameTimer); worldImportNameDirty = false; worldImportDesiredName = state.importPreview?.candidate.generated ? state.importPreview.candidate.name : ""; worldImportName.value = worldImportDesiredName; worldImportNameTouched = false;
    } else if (op === "name-import") {
      const sent = validWorldName(worldImportDesiredName) ? worldImportDesiredName : "";
      if (state.importPreview?.candidate.name === sent) worldImportNameDirty = false;
    }
  }
  if (worldTarget && (worldTarget.activeId !== state.activeId || worldTarget.revision !== state.revision)) worldDialog.close();
  if (worldImportTarget && (worldImportTarget.activeId !== state.activeId || worldImportTarget.revision !== state.revision)) worldImportDialog.close();
  LauncherI18n.bind(document.getElementById("world-notice"), "textContent", () => (state.recoveryRequired ? worldNotices["recovery-required"] : worldMessage(state.notice)));
  if (worldDialog.open) {
    LauncherI18n.bind(document.getElementById("world-dialog-notice"), "textContent", () => (worldMessage(state.notice)));
    if (["purge","empty-trash"].includes(worldTarget?.op)) {
      if (state.purgePreview && worldTarget.op === "empty-trash") LauncherI18n.bind(document.getElementById("world-dialog-target"), "textContent", () => (LauncherI18n.t("{0} worlds in Trash · {1} MiB", [state.purgePreview.count, (state.purgePreview.size / 1048576).toFixed(2)])));
      const notes = [...(state.purgeIssues || []),...(state.purgePreview?.retainedBackups || [])];
      document.getElementById("world-purge-issues").replaceChildren(...notes.map(issue => worldNode("p",`${issue.name || issue.key || LauncherI18n.t("Target data")} · ${worldMessage(issue.reason)}`)));
      if (op === "prepare-purge") { updateWorldControls(); document.getElementById("world-cancel").focus(); }
    }
    if (op === "preview" && state.preview) document.getElementById("world-icon-preview").src = state.preview.icon;
    if (["saved","unchanged","recovered","deleted","restored","purged"].includes(state.notice)) worldDialog.close();
  }
  if (worldImportDialog.open) { renderWorldImport(); showWorldImportNotice(state.notice,op); if (state.notice === "imported") worldImportDialog.close(); }
  renderWorlds();
  if (worldImportDialog.open) {
    if (op === "begin-import") document.getElementById("world-import-body").focus();
    else if (op === "select-import-body" && state.notice !== "cancelled" && state.importPreview?.candidate.worldFile && state.importPreview.candidate.generated) worldImportName.focus();
    if (worldImportNameDirty) worldImportNameTimer = setTimeout(flushImportName,50);
  }
});
window.addEventListener("launcher-account-result", event => {
  const state = event.detail?.account;
  if (state && worldSnapshot && (state.activeId !== worldSnapshot.activeId || worldTarget)) {
    if (!worldPending) { if (worldDialog.open) worldDialog.close(); if (worldImportDialog.open) worldImportDialog.close(); } worldSnapshot = null; worldVisible = false; renderWorlds(); updateWorldView();
  }
});
let worldManagerGeneration, worldEditingAllowed = false;
window.addEventListener("launcher-core-state", () => {
  const generation = window.launcherCoreState?.managerGeneration;
  const canEdit = window.launcherCoreState?.canEditWorlds === true;
  if (canEdit && !worldEditingAllowed) worldDirty = true;
  worldEditingAllowed = canEdit;
  if (generation !== worldManagerGeneration) {
    worldManagerGeneration = generation; worldPending = 0; worldPendingOp = ""; worldSnapshot = null; worldTarget = null; worldImportTarget = null; worldVisible = false;
    clearTimeout(worldImportNameTimer); worldDialog.close(); worldImportDialog.close(); renderWorlds();
  }
  updateWorldView();
});
document.addEventListener("visibilitychange", updateWorldView);

window.addEventListener("launcher-library-change", event => {
  if (event.detail?.name !== "world") return;
  worldDirty = true; updateWorldView();
});
window.addEventListener("launcher-live-state", () => { if (selectedPage === "world" && !document.hidden) updateWorldView(); });

window.addEventListener("launcher-language-changed", () => { worldRendered = ""; renderWorlds(); renderWorldImport(); });
