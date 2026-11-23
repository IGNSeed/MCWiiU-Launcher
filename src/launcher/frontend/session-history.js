"use strict";

window.SessionHistoryUI = (() => {
  const page = document.getElementById("session-history");
  const chart = document.getElementById("history-chart");
  const year = document.getElementById("history-year"), month = document.getElementById("history-month");
  const modes = [...document.querySelectorAll("[data-history-mode]")];
  const messages = {
    get "history-unavailable"() { return LauncherI18n.t("Cannot read session history. Existing history is preserved; new sessions cannot be saved."); },
    get "backup-read-only"() { return LauncherI18n.t("History validation failed. Showing a protected backup; new sessions cannot be saved."); },
    get "save-failed"() { return LauncherI18n.t("Cannot save session history. You may continue playing, but unsaved time may be lost on exit."); },
    get "tracking-failed"() { return LauncherI18n.t("Cannot continue tracking playtime. Saved history is preserved."); }
  };
  let mode = "daily", anchor = "", selectedYear = 0, selectedMonth = 0;
  let state = null, sequence = 0, pending = null, timer = 0, bars = [], chartKey = "", datesKey = "", cardsKey = "";
  const namespace = "http://www.w3.org/2000/svg";
  const svg = document.createElementNS(namespace,"svg");
  svg.setAttribute("viewBox","0 0 900 240"); LauncherI18n.bind(svg, "aria-label", () => LauncherI18n.t("Playtime by period"), true);
  chart.append(svg);
  const tooltip = document.createElement("div"); tooltip.className = "history-tooltip"; tooltip.id = "history-tooltip"; tooltip.setAttribute("role","tooltip"); tooltip.hidden = true; chart.append(tooltip);
  for (let i = 0; i <= 4; i++) {
    const line = document.createElementNS(namespace,"line");
    for (const [key,value] of Object.entries({x1:0,x2:900,y1:10+i*48,y2:10+i*48})) line.setAttribute(key,value);
    line.classList.add("history-gridline"); svg.append(line);
  }
  function iso(date) { return date.toISOString().slice(0,10); }
  function date(text) { return new Date(text + "T12:00:00Z"); }
  function fullDate(text) { return new Intl.DateTimeFormat(LauncherI18n.language, {year:"numeric",month:"short",day:"numeric",timeZone:"UTC"}).format(date(text)); }
  function duration(seconds, precise = true) {
    const h = Math.floor(seconds/3600), m = Math.floor(seconds/60)%60, s = seconds%60;
    return LauncherI18n.t("{0}h {1}m {2}", [h.toLocaleString(LauncherI18n.language), String(m).padStart(2,"0"), precise ? String(s).padStart(2,"0")+LauncherI18n.t("s") : ""]);
  }
  function period(bucket) { return bucket.start === bucket.end ? fullDate(bucket.start) : `${fullDate(bucket.start)} ～ ${fullDate(bucket.end)}`; }
  function visible() { return selectedPage === "session-history" && !page.hidden && !page.inert && !document.hidden; }
  function requestData() { return {mode,anchor,year:selectedYear,month:selectedMonth}; }
  function request() {
    if (!visible()) return;
    if (pending && performance.now() - pending.time < 3000) return;
    const query = JSON.stringify(requestData());
    pending = {id:++sequence,query,time:performance.now()};
    windowCommand(`history:query:${sequence}:${query}`);
  }
  function notice(value) {
    const banner = document.getElementById("history-global-notice");
    const text = messages[value] || "";
    if (banner.textContent !== text) LauncherI18n.bind(banner, "textContent", () => messages[value] || "");
    banner.hidden = !text;
    const badge = document.getElementById("history-badge");
    badge.hidden = !text; LauncherI18n.bind(badge, "title", () => messages[value] || "");
  }
  function showTooltip(index) {
    if (!state?.buckets[index]) return;
    const bucket = state.buckets[index];
    LauncherI18n.bind(tooltip, "textContent", () => `${period(bucket)} · ${duration(bucket.seconds)}`); tooltip.hidden = false;
    LauncherI18n.bind(document.getElementById("history-chart-detail"), "textContent", () => tooltip.textContent);
  }
  function drill(index) {
    const bucket = state?.buckets[index]; if (!bucket) return;
    const [y,m] = bucket.start.split("-").map(Number);
    if (mode === "yearly") { mode = "monthly"; anchor = `${y}-12-31`; selectedYear = y; selectedMonth = 1; }
    else if (mode === "monthly") { selectedYear = y; selectedMonth = m; mode = "daily"; anchor = bucket.end; }
    else { selectedYear = y; selectedMonth = m; }
    year.value = selectedYear; month.value = selectedMonth;
    pending = null; syncModes(); request();
    if (mode === "daily") document.getElementById("history-detail-range").scrollIntoView({block:"nearest",behavior:"smooth"});
  }
  function graph() {
    const graphWidth = Math.max(260,chart.clientWidth);
    const key = LauncherI18n.language + mode + graphWidth + JSON.stringify(state.buckets); if (key === chartKey) return;
    chartKey = key; tooltip.hidden = true;
    svg.setAttribute("viewBox",`0 0 ${graphWidth} 240`);
    svg.querySelectorAll("line").forEach(line => line.setAttribute("x2",graphWidth));
    const buckets = state.buckets, width = graphWidth/buckets.length;
    const maximum = Math.max(3600,...buckets.map(item => item.seconds));
    const axis = document.getElementById("history-axis");
    axis.replaceChildren(...[4,3,2,1,0].map(i => { const value = document.createElement("span"); LauncherI18n.bind(value, "textContent", () => `${Number((maximum/3600*i/4).toFixed(1))}h`); return value; }));
    if (bars.length !== buckets.length) {
      for (const bar of bars) bar.group.remove();
      bars = buckets.map((_,index) => {
        const group = document.createElementNS(namespace,"g"); group.classList.add("history-bar"); group.setAttribute("role","button"); group.setAttribute("tabindex", index ? "-1" : "0"); group.setAttribute("aria-describedby","history-tooltip");
        const hit = document.createElementNS(namespace,"rect"); hit.classList.add("history-hit");
        const rect = document.createElementNS(namespace,"rect"); rect.classList.add("history-bar-value");
        const label = document.createElementNS(namespace,"text"); label.setAttribute("y","229"); label.setAttribute("text-anchor","middle");
        const title = document.createElementNS(namespace,"title");
        group.append(title,hit,rect,label); svg.append(group);
        group.addEventListener("mouseenter",() => showTooltip(index)); group.addEventListener("mouseleave",() => { tooltip.hidden = true; });
        group.addEventListener("focus",() => { bars.forEach((bar,i) => bar.group.setAttribute("tabindex",i === index ? "0" : "-1")); showTooltip(index); });
        group.addEventListener("blur",() => { tooltip.hidden = true; }); group.addEventListener("click",() => drill(index));
        group.addEventListener("keydown",event => {
          let next;
          if (event.key === "ArrowRight") next = Math.min(index+1,bars.length-1);
          else if (event.key === "ArrowLeft") next = Math.max(index-1,0);
          else if (event.key === "Home") next = 0;
          else if (event.key === "End") next = bars.length-1;
          else if (event.key === "Enter" || event.key === " ") { event.preventDefault(); drill(index); return; }
          if (next !== undefined) { event.preventDefault(); bars[next].group.focus(); }
        });
        return {group,hit,rect,label,title};
      });
    }
    buckets.forEach((bucket,index) => {
      const bar = bars[index], height = bucket.seconds/maximum*192;
      bar.hit.setAttribute("x",index*width); bar.hit.setAttribute("y",0); bar.hit.setAttribute("width",width); bar.hit.setAttribute("height",238);
      bar.rect.setAttribute("x",index*width+width*.2); bar.rect.setAttribute("width",width*.6); bar.rect.setAttribute("y",202-Math.max(height,2)); bar.rect.setAttribute("height",Math.max(height,2));
      bar.rect.classList.toggle("history-zero",!bucket.seconds);
      const [y,m,d] = bucket.start.split("-").map(Number);
      bar.label.setAttribute("x",index*width+width/2);
      LauncherI18n.bind(bar.label, "textContent", () => mode === "yearly" ? y : mode === "monthly" ? LauncherI18n.t("Month {0}", [m]) : index%Math.ceil(buckets.length/6) === 0 || index === buckets.length-1 ? `${m}/${d}` : "");
      LauncherI18n.bind(bar.group, "aria-label", () => `${period(bucket)}、${duration(bucket.seconds)}`, true);
      LauncherI18n.bind(bar.title, "textContent", () => LauncherI18n.t("{0}\nPlaytime: {1}", [period(bucket), duration(bucket.seconds)]));
    });
    const selected = bars.findIndex(bar => bar.group === document.activeElement || bar.group.matches(":hover"));
    if (selected >= 0) showTooltip(selected);
    LauncherI18n.bind(document.getElementById("history-range"), "textContent", () => `${buckets[0].start} ～ ${buckets.at(-1).end}`);
  }
  function render() {
    notice(state.notice);
    LauncherI18n.bind(document.getElementById("history-status"), "textContent", () => state.readOnly ? LauncherI18n.t("Check the storage location. Existing history is preserved.") : state.active ? LauncherI18n.t("Tracking · Session {0} · Saved total {1}", [duration(state.sessionSeconds), duration(state.savedSeconds)]) : state.recovered ? LauncherI18n.t("Recovered to the last saved checkpoint.") : "");
    const values = [[LauncherI18n.t("Today"),state.todaySeconds],[LauncherI18n.t("This Week"),state.weekSeconds],[LauncherI18n.t("This Month"),state.monthSeconds],[LauncherI18n.t("This Year"),state.yearSeconds],[LauncherI18n.t("Total Playtime"),state.totalSeconds]];
    const key = LauncherI18n.language + JSON.stringify(values);
    if (key !== cardsKey) {
      cardsKey = key;
      const cards = document.getElementById("history-cards");
      if (!cards.children.length) for (const [name] of values) { const card = document.createElement("div"); card.className = "history-card"; const label = document.createElement("span"); LauncherI18n.bind(label, "textContent", () => name); const value = document.createElement("strong"); card.append(label,value); cards.append(card); }
      values.forEach(([,seconds],index) => { const card = cards.children[index]; LauncherI18n.bind(card.lastElementChild, "textContent", () => duration(seconds,false)); LauncherI18n.bind(card, "title", () => duration(seconds)); });
    }
    graph();
    document.getElementById("history-empty").hidden = state.totalSeconds > 0 || state.active || state.readOnly;
    const details = `${state.year}-${state.month}:`+JSON.stringify(state.dates);
    if (details !== datesKey) {
      datesKey = details;
      const rows = document.getElementById("history-dates");
      while (rows.children.length > state.dates.length) rows.lastChild.remove();
      state.dates.forEach((bucket,index) => {
        let row = rows.children[index]; if (!row) { row = document.createElement("tr"); row.append(document.createElement("th"),document.createElement("td")); row.firstChild.scope = "row"; rows.append(row); }
        LauncherI18n.bind(row.firstChild, "textContent", () => fullDate(bucket.start)); LauncherI18n.bind(row.lastChild, "textContent", () => duration(bucket.seconds));
      });
      LauncherI18n.bind(document.getElementById("history-detail-range"), "textContent", () => LauncherI18n.t("{0}-{1} · {2} days", [state.year, state.month, state.dates.length]));
    }
  }
  window.addEventListener("launcher-language-changed", () => {
    chartKey = datesKey = cardsKey = "";
    document.getElementById("history-cards").replaceChildren();
    for (let i = 1; i <= 12; i++) month.options[i - 1].textContent = LauncherI18n.t("Month {0}", [i]);
    if (state) render();
  });
  function syncModes() { modes.forEach(button => { const selected = button.dataset.historyMode === mode; button.setAttribute("aria-selected",String(selected)); button.tabIndex = selected ? 0 : -1; }); }
  modes.forEach((button,index) => {
    button.addEventListener("click",() => { mode = button.dataset.historyMode; syncModes(); pending = null; request(); });
    button.addEventListener("keydown",event => { let next; if (event.key === "ArrowRight") next = (index+1)%4; else if (event.key === "ArrowLeft") next = (index+3)%4; else if (event.key === "Home") next = 0; else if (event.key === "End") next = 3; if (next === undefined) return; event.preventDefault(); modes[next].click(); modes[next].focus(); });
  });
  for (let i = 1; i <= 12; i++) month.add(new Option(LauncherI18n.t("Month {0}", [i]),i));
  function detailsChanged() { if (!year.checkValidity() || !year.value) return; selectedYear = Number(year.value); selectedMonth = Number(month.value); pending = null; request(); }
  year.addEventListener("change",detailsChanged); month.addEventListener("change",detailsChanged);
  function move(direction) {
    if (!state) return;
    const current = date(anchor || state.today);
    if (mode === "daily" || mode === "weekly") current.setUTCDate(current.getUTCDate()+direction*(mode === "daily" ? 30 : 84));
    else if (mode === "monthly") { current.setUTCDate(1); current.setUTCMonth(current.getUTCMonth()+direction*12); }
    else { current.setUTCDate(1); current.setUTCFullYear(current.getUTCFullYear()+direction*5); }
    if (current.getUTCFullYear() < 1602 || current.getUTCFullYear() >= 9998) return;
    anchor = iso(current); pending = null; request();
  }
  document.getElementById("history-prev").addEventListener("click",() => move(-1));
  document.getElementById("history-next").addEventListener("click",() => move(1));
  document.getElementById("history-current").addEventListener("click",() => { anchor = ""; pending = null; request(); });
  document.getElementById("history-detail-current").addEventListener("click",() => { const today = state?.today; if (!today) return; [selectedYear,selectedMonth] = today.split("-").map(Number); year.value = selectedYear; month.value = selectedMonth; pending = null; request(); });
  window.addEventListener("launcher-history-state",event => {
    const reply = event.detail; if (!pending || reply.requestId !== pending.id) return;
    const accepted = pending.query === JSON.stringify(requestData()); pending = null;
    if (!accepted) { request(); return; }
    if (!reply.state.buckets) { LauncherI18n.bind(document.getElementById("history-status"), "textContent", () => LauncherI18n.t("Could not load history.")); return; }
    state = reply.state;
    if (!selectedYear) { selectedYear = state.year; selectedMonth = state.month; year.value = selectedYear; month.value = selectedMonth; }
    if (visible()) render();
    clearInterval(timer); timer = visible() && state.active ? setInterval(request,1000) : 0;
  });
  window.addEventListener("launcher-history-notice",event => notice(event.detail));
  function update() { clearInterval(timer); timer = 0; if (visible()) { pending = null; request(); } else tooltip.hidden = true; }
  window.updateSessionHistoryView = update;
  new MutationObserver(update).observe(page,{attributes:true,attributeFilter:["hidden","inert"]});
  new ResizeObserver(() => { if (visible() && state) graph(); }).observe(chart);
  document.addEventListener("visibilitychange",update);
  window.addEventListener("launcher-core-state",() => { if (visible() && Boolean(state?.active) !== (window.launcherCoreState?.phase === "running")) request(); });
  window.addEventListener("pagehide",() => { clearInterval(timer); timer = 0; pending = null; });
  return {duration,get state() { return state; }};
})();
