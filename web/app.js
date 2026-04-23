"use strict";

/* ═══════════════════════════════════════════════════════════════════════
   Container Internals Simulator — dashboard client
   Polls every POLL_MS, updates views reactively.
   ═══════════════════════════════════════════════════════════════════════ */

const API     = "";
const POLL_MS = 2000;
const TICKS   = 30;

/* ── chart history ──────────────────────────────────────────────────── */
const hist = { running: Array(TICKS).fill(0), events: Array(TICKS).fill(0) };
let prevEventsTotal = 0;
let chartRunning, chartEvents;

/* ── shared state ───────────────────────────────────────────────────── */
let firingSet       = new Set();
let lastContainers  = [];
let lastStats       = {};
let lastMetrics     = {};
let selectedLogId   = "";

/* ── DOM helpers ────────────────────────────────────────────────────── */
const $  = id => document.getElementById(id);
const setText = (id, v) => { const el=$(id); if(el) el.textContent = v ?? "—"; };
const setHtml = (id, h) => { const el=$(id); if(el) el.innerHTML = h; };

function escHtml(s) {
  return String(s ?? "")
    .replace(/&/g,"&amp;").replace(/</g,"&lt;").replace(/>/g,"&gt;");
}

function fmtUptime(s) {
  if (!s || s < 0) return "—";
  if (s < 60)   return `${s}s`;
  if (s < 3600) return `${Math.floor(s/60)}m ${s%60}s`;
  return `${Math.floor(s/3600)}h ${Math.floor((s%3600)/60)}m`;
}

function icon(name, cls="") {
  return `<i data-lucide="${name}" class="${cls}"></i>`;
}

function refreshIcons() {
  if (typeof lucide !== "undefined") lucide.createIcons();
}

/* ── badge + event helpers ──────────────────────────────────────────── */
function badge(state) {
  const cls = { RUNNING:"running", STOPPED:"stopped", PAUSED:"paused", CREATED:"created" }[state] ?? "created";
  return `<span class="badge badge-${cls}">${state}</span>`;
}

function evClass(type) {
  if (/STOP|DELETE|OOM|ALERT_FIRED/.test(type)) return "is-stop";
  if (/CREATE|START|BUILT/.test(type))           return "is-create";
  if (/^ORCH/.test(type))                        return "is-orch";
  if (/^SCHED/.test(type))                       return "is-sched";
  if (/^ALERT/.test(type))                       return "is-alert";
  return "";
}

/* ── resource bar ───────────────────────────────────────────────────── */
function rssBar(cid) {
  const s = lastStats[cid];
  if (!s || s.rss_mb == null) return `<span class="u-muted">—</span>`;
  const mb   = s.rss_mb;
  const pct  = Math.min(100, (mb / 512) * 100);
  const warn = pct > 60 ? (pct > 85 ? "crit" : "warn") : "";
  return `
    <div class="res-bar-wrap">
      <div class="res-bar-label">${mb} MB</div>
      <div class="res-bar-track">
        <div class="res-bar-fill ${warn}" style="width:${pct.toFixed(1)}%"></div>
      </div>
    </div>`;
}

/* ── tab navigation ─────────────────────────────────────────────────── */
function initNav() {
  document.querySelectorAll(".tab").forEach(tab => {
    tab.addEventListener("click", () => {
      const view = tab.dataset.view;
      document.querySelectorAll(".tab").forEach(t => t.classList.remove("active"));
      document.querySelectorAll(".view").forEach(v => v.classList.remove("active-view"));
      tab.classList.add("active");
      const el = $(`view-${view}`);
      if (el) el.classList.add("active-view");

      if (view === "metrics") renderMetricsView(lastMetrics);
      if (view === "images")  renderImages(lastContainers);
      if (view === "logs")    renderLogSelector(lastContainers);
      refreshIcons();
    });
  });
}

/* ── theme helper ───────────────────────────────────────────────────── */
function cssVar(name) {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}

function cssVarAlpha(name, alpha) {
  const hex = cssVar(name);
  const r = parseInt(hex.slice(1,3),16);
  const g = parseInt(hex.slice(3,5),16);
  const b = parseInt(hex.slice(5,7),16);
  return `rgba(${r},${g},${b},${alpha})`;
}

/* ── chart setup ────────────────────────────────────────────────────── */
function buildChartBase() {
  const tickColor = cssVar("--text-dim");
  const gridColor = cssVarAlpha("--text-dim", 0.12);
  return {
    animation: false, responsive: true, maintainAspectRatio: false,
    plugins: { legend: { display: false } },
    scales: {
      x: { display: false },
      y: {
        min: 0,
        ticks: { color: tickColor, maxTicksLimit: 4 },
        grid:  { color: gridColor },
      },
    },
  };
}

function makeDataset(data, opts) {
  return { data: [...data], pointRadius: 0, ...opts };
}

function initCharts() {
  const green    = cssVar("--green");
  const greenBg  = cssVarAlpha("--green", 0.15);
  const accent   = cssVar("--purple");
  const accentBg = cssVarAlpha("--purple", 0.18);

  chartRunning = new Chart($("chart-running").getContext("2d"), {
    type: "line",
    data: {
      labels: hist.running.map(()=>""),
      datasets: [makeDataset(hist.running, {
        borderColor: green, backgroundColor: greenBg,
        borderWidth: 2, fill: true, tension: 0.3,
      })],
    },
    options: buildChartBase(),
  });

  chartEvents = new Chart($("chart-events").getContext("2d"), {
    type: "bar",
    data: {
      labels: hist.events.map(()=>""),
      datasets: [makeDataset(hist.events, {
        backgroundColor: accentBg, borderColor: accent, borderWidth: 1,
      })],
    },
    options: buildChartBase(),
  });
}

function pushTick(chart, history, value) {
  history.push(value);
  if (history.length > TICKS) history.shift();
  chart.data.datasets[0].data = [...history];
  chart.data.labels            = history.map(()=>"");
  chart.update();
}

/* ── stop action ────────────────────────────────────────────────────── */
async function stopContainer(id, btn) {
  btn.disabled    = true;
  btn.textContent = "Stopping…";
  try {
    await fetch(`${API}/api/containers/${encodeURIComponent(id)}/stop`, { method:"POST" });
  } catch(_) {}
  setTimeout(() => {
    btn.disabled  = false;
    btn.innerHTML = icon("square","icon-xs") + " Stop";
    refreshIcons();
  }, 2000);
}
window.stopContainer = stopContainer;

/* ── VIEW: Containers ───────────────────────────────────────────────── */
function renderContainers(containers) {
  const running = containers.filter(c => c.state === "RUNNING").length;
  const countEl = $("container-count");
  if (countEl) {
    countEl.textContent = `${containers.length} total · ${running} running`;
    countEl.classList.remove("u-hidden");
  }

  if (containers.length === 0) {
    setHtml("container-list", `
      <div class="empty-state">
        ${icon("package","icon-xl")}
        <p>No containers — start one from the CLI</p>
      </div>`);
    refreshIcons();
    return;
  }

  const header = `
    <div class="ctr-header">
      <span>Name / ID</span>
      <span>Status</span>
      <span>Uptime</span>
      <span>Memory</span>
      <span>IP / Ports</span>
      <span>Action</span>
    </div>`;

  const rows = containers.map(c => {
    const alertDot = firingSet.has(c.id) ? `<span class="alert-dot" title="Alert firing"></span>` : "";
    const iconCls  = c.state === "RUNNING" ? "is-running" : c.state === "PAUSED" ? "is-paused" : "";
    const action   = c.state === "RUNNING"
      ? `<button class="btn-stop" onclick="stopContainer('${escHtml(c.id)}',this)">${icon("square","icon-xs")} Stop</button>`
      : `<span class="u-muted">${c.exit_code ? "exit " + c.exit_code : "—"}</span>`;

    return `
      <div class="ctr-row">
        <div class="ctr-name-cell">
          <div class="ctr-icon ${iconCls}">${icon("box","icon-xs")}</div>
          <div class="ctr-name-stack">
            <div class="ctr-name">${escHtml(c.name)}${alertDot}</div>
            <div class="ctr-id-cmd">${escHtml(c.id)} · ${escHtml(c.command)}</div>
          </div>
        </div>
        <div>${badge(c.state)}</div>
        <div class="ctr-uptime">${c.uptime || "—"}</div>
        <div>${rssBar(c.id)}</div>
        <div class="ctr-ip">${c.ip || "—"}${c.ports ? `<br><span style="font-size:10px;opacity:.7">${escHtml(c.ports)}</span>` : ""}</div>
        <div>${action}</div>
      </div>`;
  });

  setHtml("container-list", header + rows.join(""));
  refreshIcons();
}

/* ── VIEW: Images ───────────────────────────────────────────────────── */
function renderImages(containers) {
  const seen = new Map();
  containers.forEach(c => {
    const img = c.image || "rootfs";
    if (!seen.has(img)) seen.set(img, { count: 0, running: 0 });
    seen.get(img).count++;
    if (c.state === "RUNNING") seen.get(img).running++;
  });

  if (seen.size === 0) {
    setHtml("image-list", `
      <div class="empty-state">
        ${icon("layers","icon-xl")}
        <p>No image data yet — start a container</p>
      </div>`);
    refreshIcons();
    return;
  }

  const header = `
    <div class="img-row img-header">
      <span>Image</span><span>Containers</span><span>Running</span>
    </div>`;

  const rows = [...seen.entries()].map(([img, s]) => `
    <div class="img-row">
      <div class="img-name">${escHtml(img)}</div>
      <div>${s.count}</div>
      <div>${s.running > 0 ? `<span style="color:var(--green);font-weight:600">${s.running}</span>` : "0"}</div>
    </div>`);

  setHtml("image-list", header + rows.join(""));
}

/* ── VIEW: Events ───────────────────────────────────────────────────── */
function renderEvents(events) {
  const evCountEl = $("event-count");
  if (evCountEl) {
    evCountEl.textContent = `${events.length}`;
    evCountEl.classList.remove("u-hidden");
  }

  if (events.length === 0) {
    setHtml("event-list", `
      <div class="empty-state">
        ${icon("bell","icon-xl")}
        <p>No events yet</p>
      </div>`);
    refreshIcons();
    return;
  }

  const rows = [...events].reverse().map(e => {
    const t      = e.timestamp ? e.timestamp.slice(11,19) : "—";
    const detail = [e.container_id, e.detail].filter(Boolean).join(" · ");
    return `
      <div class="ev-row">
        <div class="ev-time">${t}</div>
        <div class="ev-type ${evClass(e.type)}">${escHtml(e.type)}</div>
        <div class="ev-detail">${detail ? escHtml(detail) : ""}</div>
      </div>`;
  });

  setHtml("event-list", rows.join(""));
}

async function fetchLogPreview(id) {
  const statusEl = $("log-status");

  if (!id) {
    if (statusEl) {
      statusEl.textContent = "";
      statusEl.classList.add("u-hidden");
    }
    setHtml("log-output", `
      <div class="empty-state">
        ${icon("scroll-text","icon-xl")}
        <p>Select a container with captured logs</p>
      </div>`);
    refreshIcons();
    return;
  }

  try {
    const res = await fetch(`${API}/api/containers/${encodeURIComponent(id)}/logs?n=40`);
    if (!res.ok) throw new Error("request failed");

    const data = await res.json();
    const lines = Array.isArray(data.lines) ? data.lines : [];

    if (statusEl) {
      statusEl.textContent = `${lines.length} lines`;
      statusEl.classList.remove("u-hidden");
    }

    if (!data.log_path) {
      setHtml("log-output", `
        <div class="empty-state">
          ${icon("file-x","icon-xl")}
          <p>This container has no captured log file yet</p>
        </div>`);
      refreshIcons();
      return;
    }

    if (lines.length === 0) {
      setHtml("log-output", `
        <div class="empty-state">
          ${icon("file-text","icon-xl")}
          <p>Log file exists, but there is no output yet</p>
        </div>`);
      refreshIcons();
      return;
    }

    setHtml("log-output", `
      <div class="log-meta">${escHtml(data.log_path)}</div>
      <pre class="log-pre">${escHtml(lines.join("\n"))}</pre>`);
  } catch (_) {
    if (statusEl) {
      statusEl.textContent = "";
      statusEl.classList.add("u-hidden");
    }
    setHtml("log-output", `
      <div class="empty-state">
        ${icon("wifi-off","icon-xl")}
        <p>Could not load logs right now</p>
      </div>`);
    refreshIcons();
    return;
  }

  refreshIcons();
}

function renderLogSelector(containers) {
  const select = $("log-select");
  if (!select) return;

  const candidates = containers.filter(c => c.log_path);
  if (!selectedLogId || !candidates.some(c => c.id === selectedLogId)) {
    selectedLogId = candidates[0]?.id || "";
  }

  select.innerHTML = candidates.length
    ? candidates.map(c => `
        <option value="${escHtml(c.id)}" ${c.id === selectedLogId ? "selected" : ""}>
          ${escHtml(c.name)} · ${escHtml(c.id)}
        </option>`).join("")
    : `<option value="">No containers with logs</option>`;

  select.disabled = candidates.length === 0;
  fetchLogPreview(selectedLogId);
}

/* ── VIEW: Metrics ──────────────────────────────────────────────────── */
function renderMetricsView(m) {
  if (!m || !Object.keys(m).length) return;

  const uptime = m.uptime_seconds ?? 0;
  const avg    = m.startup_avg_ms ? m.startup_avg_ms + " ms" : "—";

  const rows = [
    ["containers_started_total",  m.containers_started],
    ["containers_stopped_total",  m.containers_stopped],
    ["containers_deleted_total",  m.containers_deleted],
    ["containers_paused_total",   m.containers_paused],
    ["exec_launches_total",       m.exec_launches],
    ["oom_kills_total",           m.oom_kills],
    ["images_built_total",        m.images_built],
    ["images_removed_total",      m.images_removed],
    ["scheduler_toggles_total",   m.sched_toggles],
    ["events_total",              m.events_total],
    ["startup_latency_avg_ms",    avg],
    ["startup_latency_max_ms",    m.startup_max_ms != null ? m.startup_max_ms + " ms" : "—"],
    ["memory_highwater_mb",       m.mem_highwater_mb != null ? m.mem_highwater_mb + " MB" : "—"],
    ["simulator_uptime",          fmtUptime(uptime)],
  ];

  const html = rows.map(([k, v]) => `
    <tr><td>${escHtml(k)}</td><td>${escHtml(String(v ?? "—"))}</td></tr>
  `).join("");

  setHtml("metrics-tbody", html);
}

/* ── Stats card + chart updates ─────────────────────────────────────── */
function renderStatCards(m, containers) {
  const running = containers.filter(c => c.state === "RUNNING").length;
  setText("m-running", running);
  setText("m-started", m.containers_started);
  setText("m-stopped", m.containers_stopped);
  setText("m-oom",     m.oom_kills);
  setText("m-avg",     m.startup_avg_ms ? m.startup_avg_ms + " ms" : "—");
  setText("m-uptime",  fmtUptime(m.uptime_seconds));

  if (chartRunning) pushTick(chartRunning, hist.running, running);
  if (chartEvents) {
    const delta = Math.max(0, (m.events_total ?? 0) - prevEventsTotal);
    pushTick(chartEvents, hist.events, delta);
  }
  prevEventsTotal = m.events_total ?? 0;
}

/* ── Alerts ─────────────────────────────────────────────────────────── */
function renderAlerts(alerts) {
  const firing = alerts.filter(a => a.firing);
  firingSet    = new Set(firing.map(a => a.container_id));
  const banner = $("alert-banner");

  if (firing.length === 0) { banner.classList.add("u-hidden"); return; }

  banner.classList.remove("u-hidden");
  setHtml("alert-content",
    firing.map(a =>
      `<strong>${escHtml(a.container_id)}</strong> ${escHtml(a.metric)} &gt; ${escHtml(a.threshold)}`
    ).join("&ensp;|&ensp;")
  );
}

/* ── Poll loop ──────────────────────────────────────────────────────── */
async function poll() {
  try {
    const [cRes, eRes, mRes, aRes, sRes] = await Promise.all([
      fetch(`${API}/api/containers`),
      fetch(`${API}/api/events?n=100`),
      fetch(`${API}/api/metrics`),
      fetch(`${API}/api/alerts`),
      fetch(`${API}/api/stats`),
    ]);

    if (cRes.ok) lastContainers = await cRes.json();
    if (mRes.ok) lastMetrics    = await mRes.json();
    if (sRes.ok) lastStats      = await sRes.json();

    const alerts = aRes.ok ? await aRes.json() : [];

    renderAlerts(alerts);
    renderContainers(lastContainers);
    if (eRes.ok) renderEvents(await eRes.json());
    renderStatCards(lastMetrics, lastContainers);

    if ($("view-metrics")?.classList.contains("active-view"))
      renderMetricsView(lastMetrics);

    if ($("view-images")?.classList.contains("active-view"))
      renderImages(lastContainers);

    if ($("view-logs")?.classList.contains("active-view"))
      renderLogSelector(lastContainers);


  } catch(_) {
  }
}

/* ── Live clock ─────────────────────────────────────────────────────── */
function updateClock() {
  const el = $("topbar-date");
  if (!el) return;
  const now = new Date();
  el.textContent = now.toLocaleDateString(undefined, { weekday:"short", month:"short", day:"numeric" })
    + "  " + now.toLocaleTimeString();
}

/* ── Boot ───────────────────────────────────────────────────────────── */
initNav();
if ($("log-select")) {
  $("log-select").addEventListener("change", e => {
    selectedLogId = e.target.value;
    fetchLogPreview(selectedLogId);
  });
}
if (typeof Chart !== "undefined") initCharts();
updateClock();
setInterval(updateClock, 1000);
poll();
setInterval(poll, POLL_MS);
