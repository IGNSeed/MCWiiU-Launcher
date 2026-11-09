"use strict";

const friendTabs = [...document.querySelectorAll("[data-friend-tab]")];
let selectedFriendTab = "all", friendsSnapshot = null, friendsPoll = 0, friendsPending = false, friendsRendered = "", friendsCooldown = 0, friendsWasVisible = false;
const friendsMotion = new LauncherMotion.PanelTransition(document.querySelector(".friends-content"), friendTabs.map(tab => [tab.dataset.friendTab,document.getElementById(tab.getAttribute("aria-controls"))]), "all", () => {
  document.querySelector(".friends-content").scrollTop = 0;
}, () => {});
const friendsIndicator = new LauncherMotion.SlidingIndicator(document.querySelector(".friends-tabs"), document.getElementById("friends-tab-indicator"), friendTabs, () => friendTabs.find(tab => tab.dataset.friendTab === selectedFriendTab));
const friendStatus = {
  offline:"Offline", connecting:"Connecting", connected:"Connected", reconnecting:"Reconnecting",
  "authentication-failed":"Authentication Failed", "network-error":"Network Error", "server-unavailable":"Server Unavailable",
  "account-not-ready":"Account Not Ready", unavailable:"Core unavailable"
};
function friendNode(tag, text, className) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}
function renderFriends() {
  const state = friendsSnapshot;
  const connected = state?.status === "connected";
  const status = friendStatus[state?.status] || "Account Not Ready";
  document.getElementById("friends-status").textContent = state?.service && state.service !== "Offline" ? `${state.service} · ${status}` : status;
  document.getElementById("friends-refresh").disabled = !state || ["offline","account-not-ready","unavailable","connecting"].includes(state.status) || state.refreshing || Date.now() < friendsCooldown;
  document.getElementById("friends-note").textContent = state?.preferencesUnset ? "サーバーのPreferenceは未設定です。自動変更せず、取得した情報を表示します。" : "";
  updateFriendManagement();
  const incoming = connected ? state.requests.filter(request => request.incoming).length : 0;
  const badge = document.getElementById("friends-badge");
  badge.hidden = incoming === 0; badge.textContent = incoming ? String(incoming) : "";
  const key = state ? `${state.generation}:${state.revision}:${state.status}` : "empty";
  if (friendsRendered === key) return;
  friendsRendered = key;
  for (const [id, entries, request] of [
    ["friends-all", connected ? state.friends : [], false],
    ["friends-online", connected ? state.friends.filter(friend => friend.online) : [], false],
    ["friends-incoming", connected ? state.requests.filter(item => item.incoming) : [], true],
    ["friends-outgoing", connected ? state.requests.filter(item => !item.incoming) : [], true]
  ]) {
    const list = document.getElementById(id), rows = document.createDocumentFragment();
    for (const entry of entries) {
      const row = friendNode("li", undefined, "friend-row");
      const dot = friendNode("span", undefined, "friend-presence");
      if (!request && entry.online) dot.dataset.online = "true";
      dot.setAttribute("aria-hidden", "true"); row.append(dot);
      const profile = friendNode("div", undefined, "friend-profile");
      profile.append(friendNode("strong", entry.displayName || entry.accountId || String(entry.pid)), friendNode("span", entry.accountId ? `PNID · ${entry.accountId}` : `PID · ${entry.pid}`, "friend-secondary"));
      if (request && entry.message) profile.append(friendNode("p", entry.message, "friend-message"));
      row.append(profile);
      let description = request ? (entry.incoming ? "Incoming request" : "Outgoing request") : entry.online ? "Online" : "Offline";
      if (request && entry.expired) description = "Expired request";
      if (!request && entry.online && entry.titleId) description = entry.titleId === "00050000101dbe00" ? "Minecraft: Wii U Edition" : `Title ID · ${entry.titleId}`;
      row.append(friendNode("span", description, "friend-activity"));
      const actions = friendNode("div", undefined, "friend-row-actions");
      for (const [operation,label] of request ? (entry.incoming ? [["accept","Accept"],["reject","Reject"]] : [["cancel","Cancel Request"]]) : [["menu","…"]]) {
        const button = friendNode("button", label);
        button.type = "button"; button.dataset.friendOperation = operation; button.dataset.pid = String(entry.pid); button.dataset.messageId = entry.messageId || "0";
        button.disabled = !canManageFriends() || (request && (entry.manageable !== true || (operation === "accept" && entry.expired)));
        if (!request) { button.setAttribute("aria-label", `Actions for ${entry.displayName || entry.accountId}`); button.setAttribute("aria-haspopup","menu"); }
        if (request && entry.manageable !== true) button.title = "This request type cannot be managed safely.";
        actions.append(button);
      }
      row.append(actions); rows.append(row);
    }
    if (!entries.length) rows.append(friendNode("li", connected ? (request ? "No requests" : "No friends") : "接続後に最新の情報を表示します。", "friends-empty"));
    list.replaceChildren(rows);
  }
}
function validFriendsState(state) {
  const text = (value, limit) => typeof value === "string" && value.length <= limit;
  const pid = value => Number.isInteger(value) && value > 0 && value <= 0xffffffff;
  return state && Number.isSafeInteger(state.generation) && state.generation >= 0 && Number.isSafeInteger(state.revision) && state.revision >= 0 && typeof state.refreshing === "boolean" && typeof state.preferencesUnset === "boolean" &&
    Number.isInteger(state.activeId) && ["Pretendo","Offline","Custom"].includes(state.service) && Object.hasOwn(friendStatus,state.status) &&
    Array.isArray(state.friends) && state.friends.length <= 1000 && Array.isArray(state.requests) && state.requests.length <= 2000 &&
    state.friends.every(friend => pid(friend.pid) && text(friend.accountId,31) && text(friend.displayName,127) && typeof friend.online === "boolean" && text(friend.titleId,16) && (!friend.titleId || /^[0-9a-f]{16}$/.test(friend.titleId))) &&
    state.requests.every(request => pid(request.pid) && text(request.accountId,31) && text(request.displayName,127) && text(request.message,1023) && typeof request.incoming === "boolean" &&
      (request.messageId === undefined || (typeof request.messageId === "string" && /^(0|[1-9][0-9]{0,19})$/.test(request.messageId) && BigInt(request.messageId) <= 18446744073709551615n)) && (request.manageable === undefined || typeof request.manageable === "boolean") && (request.expired === undefined || typeof request.expired === "boolean")) &&
    (state.operation === undefined || (Number.isSafeInteger(state.operation.id) && state.operation.id >= 0 && text(state.operation.state,32) && text(state.operation.notice,64) && typeof state.operation.busy === "boolean")) && Number.isSafeInteger(state.connectionId) && state.connectionId >= 0;
}
function pollFriends(refresh = false) {
  if (friendsPending || document.hidden || selectedPage !== "friends" || window.launcherCoreState?.corePhase !== "ready") return;
  friendsPending = true;
  windowCommand(refresh ? "friends:refresh" : "friends:get");
}
function updateFriendsView() {
  clearTimeout(friendsPoll); friendsPoll = 0;
  if (selectedPage !== "friends" || document.hidden) { friendsPending = false; friendsWasVisible = false; if (friendDialog.open) friendDialog.close(); friendMenu.hidePopover(); return; }
  if (window.launcherCoreState?.phase === "unavailable") {
    friendsSnapshot = {generation:0,revision:0,activeId:0,service:"Offline",status:"unavailable",friends:[],requests:[]}; renderFriends(); return;
  }
  if (!friendsWasVisible) { friendsMotion.finish(); friendsIndicator.schedule(false); friendsWasVisible = true; }
  renderFriends(); pollFriends();
  friendsPoll = setTimeout(() => { friendsPending = false; updateFriendsView(); }, friendCommandPending || friendsSnapshot?.operation?.busy ? 500 : 2000);
}
window.updateFriendsView = updateFriendsView;
window.addEventListener("launcher-friends-state", event => {
  friendsPending = false;
  const state = event.detail;
  if (!validFriendsState(state)) return;
  if (accountSnapshot && (state.activeId !== accountSnapshot.activeId || state.service !== accountSnapshot.network)) return;
  if (friendsSnapshot && (state.generation < friendsSnapshot.generation || (state.generation === friendsSnapshot.generation && state.revision < friendsSnapshot.revision))) return;
  if (friendsSnapshot && (state.generation !== friendsSnapshot.generation || state.connectionId !== friendsSnapshot.connectionId)) resetFriendManagement();
  friendsSnapshot = state;
  if (selectedPage === "friends" && !document.hidden) renderFriends();
});
window.addEventListener("launcher-account-result", event => {
  const account = event.detail.account;
  if (friendsSnapshot && account && (friendsSnapshot.activeId !== account.activeId || friendsSnapshot.service !== account.network)) {
    resetFriendManagement();
    friendsSnapshot = null; friendsRendered = ""; document.getElementById("friends-badge").hidden = true; renderFriends();
  }
});
window.addEventListener("launcher-core-state", () => { if (!friendsPoll && selectedPage === "friends") updateFriendsView(); });
document.addEventListener("visibilitychange", updateFriendsView);
window.addEventListener("pagehide", () => { clearTimeout(friendsPoll); friendsPoll = 0; });
document.getElementById("friends-refresh").addEventListener("click", () => {
  if (document.getElementById("friends-refresh").disabled) return;
  friendsCooldown = Date.now() + 5000; friendsPending = false; pollFriends(true); renderFriends();
});
function selectFriendTab(name) {
  if (!friendTabs.some(tab => tab.dataset.friendTab === name)) return;
  selectedFriendTab = name;
  friendTabs.forEach(tab => { const selected = tab.dataset.friendTab === name; tab.setAttribute("aria-selected", String(selected)); tab.tabIndex = selected ? 0 : -1; });
  friendsIndicator.schedule(true); friendsMotion.switchTo(name, selectedPage !== "friends");
}
friendTabs.forEach((tab,index) => {
  tab.addEventListener("click", () => selectFriendTab(tab.dataset.friendTab));
  tab.addEventListener("keydown", event => {
    const next = event.key === "ArrowRight" ? (index + 1) % 3 : event.key === "ArrowLeft" ? (index + 2) % 3 : event.key === "Home" ? 0 : event.key === "End" ? 2 : -1;
    if (next < 0) return;
    event.preventDefault(); selectFriendTab(friendTabs[next].dataset.friendTab); friendTabs[next].focus();
  });
});

const friendDialog = document.getElementById("friend-dialog"), friendMenu = document.getElementById("friend-menu");
const friendInput = document.getElementById("friend-pnid"), friendConfirm = document.getElementById("friend-dialog-confirm");
let friendTarget = null, friendMenuTarget = null, friendFocusTarget = null, friendCommandSerial = Date.now(), friendCommandPending = 0;
const friendOperations = {
  send:["Add Friend","Enter the PNID of the person you want to add.","Send Request"],
  remove:["Remove Friend","Are you sure you want to remove this friend?","Remove Friend"],
  accept:["Accept Request","Accept this person's friend request?","Accept Request"],
  reject:["Reject Request","Are you sure you want to reject this request?","Reject Request"],
  cancel:["Cancel Request","Are you sure you want to cancel this outgoing request?","Cancel Request"]
};
const friendNotices = {
  "invalid-pnid":"Use 6–16 letters, numbers, dots, hyphens or underscores. Start and end with a letter or number; use single punctuation characters.",
  "self-request":"You cannot send a request to yourself.", "already-friends":"You are already friends.", "already-requested":"A friend request already exists. Check Requests.",
  "friend-not-found":"This friend is no longer in the current list.", "request-not-found":"This request is no longer available. Refresh the list.",
  "request-expired":"This request has expired.", "unsupported-request":"This request type cannot be managed safely.",
  "stale-session":"The account or connection changed. Refresh and try again.", "offline":"Connect to Pretendo to manage friends.",
  "not-connected":"Wait until Friends is connected.", "operation-busy":"Another operation is still in progress.", "duplicate-request":"This command has already been handled.",
  "user-not-found":"No user was found with this PNID.", "lookup-failed":"The PNID lookup failed. Check the connection and try again.",
  "server-error":"The server could not complete this operation.", "refresh-failed":"The latest server state could not be retrieved. No new change was sent.",
  "connection-lost":"Connection lost. The result may be unknown. Refresh before trying again.",
  "outcome-unknown":"The result is unknown. Checking server state; the request will not be resent automatically.",
  "sync-pending":"The server accepted the request, but the updated list is still pending. Refresh to check.",
  "state-confirmed":"The updated server state has been confirmed.", saved:"Done. The server list is up to date.",
  "invalid-command":"The operation could not be validated. Refresh and try again."
};
const friendProgress = {queued:"Preparing…",resolving:"Looking up PNID…",checking:"Checking current server state…",sent:"Request sent. Waiting for the server…",syncing:"Syncing server state…"};
function canManageFriends() {
  return friendsSnapshot?.status === "connected" && friendsSnapshot.service === "Pretendo" && !friendCommandPending && !friendsSnapshot.operation?.busy;
}
function resetFriendManagement() {
  if (friendDialog.open) friendDialog.close();
  friendMenu.hidePopover(); friendTarget = null; friendMenuTarget = null; friendCommandPending = 0;
  friendFocusTarget = null; friendInput.value = "";
  document.getElementById("friend-dialog-target").textContent = "";
  document.getElementById("friend-dialog-notice").textContent = "";
  document.getElementById("friends-management-notice").textContent = "";
}
function validFriendPnid(value) {
  const pnid = value.replace(/^ +| +$/g, "");
  return pnid.length >= 6 && pnid.length <= 16 && /^[A-Za-z0-9](?:[A-Za-z0-9_.-]*[A-Za-z0-9])$/.test(pnid) && !/[_\-.]{2}/.test(pnid);
}
function currentFriendTarget(target) {
  if (!target || !friendsSnapshot || target.generation !== friendsSnapshot.generation || target.connectionId !== friendsSnapshot.connectionId) return false;
  if (target.operation === "send") return true;
  return target.operation === "remove" ? friendsSnapshot.friends.some(entry => entry.pid === target.pid) :
    friendsSnapshot.requests.some(entry => entry.pid === target.pid && entry.messageId === target.messageId && entry.incoming === (target.operation !== "cancel") && entry.manageable && (target.operation !== "accept" || !entry.expired));
}
function updateFriendManagement() {
  const operation = friendsSnapshot?.operation;
  if (friendCommandPending && operation?.id === friendCommandPending) {
    const message = operation.notice ? friendNotices[operation.notice] || "The operation could not be completed." : friendProgress[operation.state] || "";
    document.getElementById("friends-management-notice").textContent = message;
    if (friendDialog.open) document.getElementById("friend-dialog-notice").textContent = message;
    if (!operation.busy) {
      friendCommandPending = 0;
      if (operation.state === "succeeded" && friendDialog.open) friendDialog.close();
    }
  } else if (!friendCommandPending && operation?.busy) {
    document.getElementById("friends-management-notice").textContent = friendProgress[operation.state] || "Working…";
  }
  const enabled = canManageFriends();
  document.getElementById("friends-add").disabled = !enabled;
  for (const button of document.querySelectorAll("[data-friend-operation]")) {
    const request = friendsSnapshot?.requests.find(entry => entry.pid === Number(button.dataset.pid) && entry.messageId === button.dataset.messageId);
    button.disabled = !enabled || (button.dataset.friendOperation !== "menu" && (request?.manageable !== true || (button.dataset.friendOperation === "accept" && request.expired)));
  }
  friendInput.disabled = Boolean(friendCommandPending || operation?.busy);
  friendConfirm.disabled = !enabled || !currentFriendTarget(friendTarget) || (friendTarget?.operation === "send" && !validFriendPnid(friendInput.value));
  document.getElementById("friend-form").setAttribute("aria-busy", String(Boolean(friendCommandPending || operation?.busy)));
  friendConfirm.textContent = friendCommandPending || operation?.busy ? "Working…" : friendOperations[friendTarget?.operation || "send"][2];
  document.getElementById("friend-dialog-cancel").textContent = friendCommandPending || operation?.busy ? "Close" : friendTarget?.operation === "cancel" ? "Keep Request" : "Cancel";
}
function openFriendDialog(operation, entry = {}, opener = document.activeElement) {
  if (!canManageFriends() || !Object.hasOwn(friendOperations,operation)) return;
  friendMenu.hidePopover();
  friendTarget = {operation,pid:entry.pid || 0,messageId:entry.messageId || "0",generation:friendsSnapshot.generation,connectionId:friendsSnapshot.connectionId};
  friendFocusTarget = opener;
  const [title,description,confirm] = friendOperations[operation];
  document.getElementById("friend-dialog-title").textContent = title;
  document.getElementById("friend-dialog-description").textContent = description;
  document.getElementById("friend-dialog-target").textContent = operation === "send" ? "" : `${entry.displayName || entry.accountId || entry.pid}\nPNID · ${entry.accountId || "Unavailable"}`;
  document.getElementById("friend-pnid-row").hidden = operation !== "send";
  friendInput.value = "";
  document.getElementById("friend-dialog-notice").textContent = "";
  document.getElementById("friend-dialog-cancel").textContent = operation === "cancel" ? "Keep Request" : "Cancel";
  friendConfirm.textContent = confirm;
  friendDialog.showModal(); updateFriendManagement();
  (operation === "send" ? friendInput : document.getElementById("friend-dialog-cancel")).focus();
}
document.getElementById("friends-add").addEventListener("click", () => openFriendDialog("send"));
document.querySelector(".friends-content").addEventListener("click", event => {
  const button = event.target.closest("[data-friend-operation]");
  if (!button || button.disabled || !canManageFriends()) return;
  const operation = button.dataset.friendOperation;
  const entries = operation === "menu" ? friendsSnapshot.friends : friendsSnapshot.requests;
  const entry = entries.find(item => item.pid === Number(button.dataset.pid) && (operation === "menu" || item.messageId === button.dataset.messageId));
  if (!entry) return;
  if (operation !== "menu") { openFriendDialog(operation,entry,button); return; }
  friendMenuTarget = {entry,generation:friendsSnapshot.generation,connectionId:friendsSnapshot.connectionId,opener:button};
  const bounds = button.getBoundingClientRect();
  friendMenu.showPopover();
  friendMenu.style.left = `${Math.max(8,Math.min(innerWidth - friendMenu.offsetWidth - 8,bounds.right - friendMenu.offsetWidth))}px`;
  friendMenu.style.top = `${Math.min(innerHeight - friendMenu.offsetHeight - 8,bounds.bottom + 4)}px`;
  document.getElementById("friend-menu-remove").focus();
});
document.getElementById("friend-menu-remove").addEventListener("click", () => {
  const target = friendMenuTarget;
  if (target?.generation === friendsSnapshot?.generation && target.connectionId === friendsSnapshot?.connectionId) openFriendDialog("remove",target.entry,target.opener);
});
friendMenu.addEventListener("toggle", event => { if (event.newState === "closed" && !friendDialog.open && friendMenuTarget?.opener?.isConnected) friendMenuTarget.opener.focus({preventScroll:true}); });
friendInput.addEventListener("input", () => { document.getElementById("friend-dialog-notice").textContent = validFriendPnid(friendInput.value) || !friendInput.value ? "" : friendNotices["invalid-pnid"]; updateFriendManagement(); });
document.getElementById("friend-dialog-cancel").addEventListener("click", () => friendDialog.close());
friendDialog.addEventListener("close", () => {
  if (selectedPage === "friends") (friendFocusTarget?.isConnected && !friendFocusTarget.disabled ? friendFocusTarget : document.getElementById("friend-tab-all")).focus({preventScroll:true});
  friendTarget = null;
});
friendDialog.addEventListener("keydown", event => {
  if (event.key !== "Tab") return;
  const controls = [...friendDialog.querySelectorAll("input,button")].filter(node => !node.disabled && node.getClientRects().length);
  if (!controls.length) return;
  if (event.shiftKey && document.activeElement === controls[0]) { event.preventDefault(); controls.at(-1).focus(); }
  else if (!event.shiftKey && document.activeElement === controls.at(-1)) { event.preventDefault(); controls[0].focus(); }
});
document.getElementById("friend-form").addEventListener("submit", event => {
  event.preventDefault();
  if (friendConfirm.disabled || !canManageFriends() || !currentFriendTarget(friendTarget)) return;
  const target = friendTarget;
  friendCommandPending = ++friendCommandSerial;
  const payload = {op:target.operation,generation:target.generation,connectionId:target.connectionId,pid:target.pid,messageId:target.messageId,pnid:target.operation === "send" ? friendInput.value : ""};
  document.getElementById("friend-dialog-notice").textContent = "Preparing…";
  windowCommand(`friends:command:${friendCommandPending}:${JSON.stringify(payload)}`);
  updateFriendManagement(); friendsRendered = "";
});
window.addEventListener("launcher-friends-command", event => {
  const result = event.detail;
  if (!result || result.requestId !== friendCommandPending || typeof result.notice !== "string") return;
  if (result.notice !== "queued") {
    friendCommandPending = 0;
    const message = friendNotices[result.notice] || "The operation could not be completed.";
    document.getElementById("friends-management-notice").textContent = message;
    if (friendDialog.open) document.getElementById("friend-dialog-notice").textContent = message;
  }
  updateFriendManagement(); friendsPending = false; pollFriends();
});
