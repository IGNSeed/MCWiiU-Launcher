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
      if (!request && entry.online && entry.titleId) description = entry.titleId === "00050000101dbe00" ? "Minecraft: Wii U Edition" : `Title ID · ${entry.titleId}`;
      row.append(friendNode("span", description, "friend-activity")); rows.append(row);
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
    state.requests.every(request => pid(request.pid) && text(request.accountId,31) && text(request.displayName,127) && text(request.message,1023) && typeof request.incoming === "boolean");
}
function pollFriends(refresh = false) {
  if (friendsPending || document.hidden || selectedPage !== "friends" || window.launcherCoreState?.corePhase !== "ready") return;
  friendsPending = true;
  windowCommand(refresh ? "friends:refresh" : "friends:get");
}
function updateFriendsView() {
  clearTimeout(friendsPoll); friendsPoll = 0;
  if (selectedPage !== "friends" || document.hidden) { friendsPending = false; friendsWasVisible = false; return; }
  if (window.launcherCoreState?.phase === "unavailable") {
    friendsSnapshot = {generation:0,revision:0,activeId:0,service:"Offline",status:"unavailable",friends:[],requests:[]}; renderFriends(); return;
  }
  if (!friendsWasVisible) { friendsMotion.finish(); friendsIndicator.schedule(false); friendsWasVisible = true; }
  renderFriends(); pollFriends();
  friendsPoll = setTimeout(() => { friendsPending = false; updateFriendsView(); }, 2000);
}
window.updateFriendsView = updateFriendsView;
window.addEventListener("launcher-friends-state", event => {
  friendsPending = false;
  const state = event.detail;
  if (!validFriendsState(state)) return;
  if (accountSnapshot && (state.activeId !== accountSnapshot.activeId || state.service !== accountSnapshot.network)) return;
  if (friendsSnapshot && (state.generation < friendsSnapshot.generation || (state.generation === friendsSnapshot.generation && state.revision < friendsSnapshot.revision))) return;
  friendsSnapshot = state;
  if (selectedPage === "friends" && !document.hidden) renderFriends();
});
window.addEventListener("launcher-account-result", event => {
  const account = event.detail.account;
  if (friendsSnapshot && account && (friendsSnapshot.activeId !== account.activeId || friendsSnapshot.service !== account.network)) {
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
