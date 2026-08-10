const REFRESH_MS = 3000;
let refreshTimer = null;

function fmtBytes(bytes) {
  if (bytes === null || bytes === undefined) return "--";
  const units = ["B", "KB", "MB", "GB", "TB"];
  let i = 0;
  let v = bytes;
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024;
    i++;
  }
  return `${v.toFixed(v < 10 && i > 0 ? 1 : 0)} ${units[i]}`;
}

function fmtRate(bytesPerSec) {
  return fmtBytes(bytesPerSec) + "/s";
}

function fmtTime(iso) {
  try {
    const d = new Date(iso);
    return d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
  } catch (e) {
    return iso;
  }
}

function barColor(percent) {
  if (percent >= 90) return "var(--red)";
  if (percent >= 70) return "var(--yellow)";
  return "var(--green)";
}

function setBar(fillEl, labelEl, percent, extraLabel) {
  const p = Math.max(0, Math.min(100, percent || 0));
  fillEl.style.width = p + "%";
  fillEl.style.background = barColor(p);
  labelEl.textContent = extraLabel !== undefined ? extraLabel : p.toFixed(1) + "%";
}

function renderSystem(data) {
  const cpu = data.cpu || {};
  const mem = data.memory || {};
  const net = data.network || {};

  setBar(document.getElementById("cpu-bar"), document.getElementById("cpu-percent-label"), cpu.percent);
  setBar(document.getElementById("mem-bar"), document.getElementById("mem-percent-label"), mem.percent,
    `${mem.percent?.toFixed(1) ?? "--"}% (${fmtBytes(mem.used)} / ${fmtBytes(mem.total)})`);

  const swapRow = document.getElementById("swap-row");
  if (mem.swap_total) {
    swapRow.style.display = "";
    setBar(document.getElementById("swap-bar"), document.getElementById("swap-percent-label"), mem.swap_percent,
      `${mem.swap_percent?.toFixed(1) ?? "--"}% (${fmtBytes(mem.swap_used)} / ${fmtBytes(mem.swap_total)})`);
  } else {
    swapRow.style.display = "none";
  }

  document.getElementById("cpu-temp").textContent = cpu.temp_c !== null && cpu.temp_c !== undefined
    ? `${cpu.temp_c}°C` : "n/a";
  document.getElementById("net-down").textContent = fmtRate(net.recv_rate_bps);
  document.getElementById("net-up").textContent = fmtRate(net.sent_rate_bps);

  const loadEl = document.getElementById("load-avg");
  if (cpu.load_avg) {
    loadEl.textContent = `Load avg: ${cpu.load_avg.map(v => v.toFixed(2)).join(" / ")}  ·  Total ↓ ${fmtBytes(net.total_recv)} / ↑ ${fmtBytes(net.total_sent)}`;
  } else {
    loadEl.textContent = `Total ↓ ${fmtBytes(net.total_recv)} / ↑ ${fmtBytes(net.total_sent)}`;
  }
}

function renderDisks(disks) {
  const container = document.getElementById("disks-container");
  if (!disks || disks.length === 0) {
    container.innerHTML = '<p class="muted">No mounted disks found.</p>';
    return;
  }
  container.innerHTML = disks.map(d => `
    <div class="disk-row">
      <div class="disk-top">
        <span class="disk-mount">${d.mount}</span>
        <span>${d.percent.toFixed(1)}%</span>
      </div>
      <div class="bar"><div class="bar-fill" style="width:${d.percent}%; background:${barColor(d.percent)}"></div></div>
      <div class="disk-detail">${fmtBytes(d.used)} used / ${fmtBytes(d.total)} total · ${d.fstype} · ${d.device}</div>
    </div>
  `).join("");
}

function renderSerial(ports) {
  const container = document.getElementById("serial-container");
  if (!ports || ports.length === 0) {
    container.innerHTML = '<p class="muted">No external serial ports connected.</p>';
    return;
  }
  container.innerHTML = ports.map(p => `
    <div class="port-row">
      <div class="port-top">
        <span>${p.device}</span>
        <span class="badge ${p.type === 'socat' ? 'badge-socat' : 'badge-usb'}">${p.type}</span>
      </div>
      <div class="port-desc">${p.description}${p.hwid ? " · " + p.hwid : ""}</div>
    </div>
  `).join("");
}

function renderLogs(logs) {
  const container = document.getElementById("logs-container");
  if (!logs || logs.length === 0) {
    container.innerHTML = '<p class="muted">No files in /tmp.</p>';
    return;
  }
  container.innerHTML = logs.map(l => `
    <div class="log-row" data-name="${encodeURIComponent(l.name)}">
      <div>
        <div class="log-name">${l.name}</div>
        <div class="log-meta">${fmtBytes(l.size)} · ${l.mtime.replace("T", " ")}</div>
      </div>
      <div class="muted">›</div>
    </div>
  `).join("");

  container.querySelectorAll(".log-row").forEach(row => {
    row.addEventListener("click", () => openLog(decodeURIComponent(row.dataset.name)));
  });
}

async function openLog(name) {
  const modal = document.getElementById("log-modal");
  const title = document.getElementById("log-modal-title");
  const body = document.getElementById("log-modal-body");
  title.textContent = name;
  body.textContent = "Loading...";
  modal.classList.remove("hidden");
  try {
    const res = await fetch(`/api/logs/${encodeURIComponent(name)}?lines=300`);
    const data = await res.json();
    body.textContent = data.content || "(empty)";
    body.scrollTop = body.scrollHeight;
  } catch (e) {
    body.textContent = "Failed to load log: " + e;
  }
}

document.getElementById("log-modal-close").addEventListener("click", () => {
  document.getElementById("log-modal").classList.add("hidden");
});
document.getElementById("log-modal").addEventListener("click", (e) => {
  if (e.target.id === "log-modal") e.target.classList.add("hidden");
});

async function refresh() {
  const dot = document.getElementById("status-dot");
  try {
    const res = await fetch("/api/data", { cache: "no-store" });
    if (!res.ok) throw new Error(res.status);
    const data = await res.json();

    renderSystem(data);
    renderDisks(data.disks);
    renderSerial(data.serial_ports);
    renderLogs(data.logs);

    document.getElementById("last-updated").textContent = "Updated " + fmtTime(data.timestamp);
    dot.classList.remove("stale");
  } catch (e) {
    dot.classList.add("stale");
    document.getElementById("last-updated").textContent = "Connection lost";
  }
}

function startPolling() {
  refresh();
  if (refreshTimer) clearInterval(refreshTimer);
  refreshTimer = setInterval(refresh, REFRESH_MS);
}

// Pause polling when tab/screen is hidden to save resources on the Pi
document.addEventListener("visibilitychange", () => {
  if (document.hidden) {
    if (refreshTimer) clearInterval(refreshTimer);
  } else {
    startPolling();
  }
});

startPolling();
