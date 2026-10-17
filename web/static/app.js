"use strict";

const $ = (selector) => document.querySelector(selector);
const icons = {
  grid: '<rect x="3" y="3" width="7" height="7" rx="1.5"/><rect x="14" y="3" width="7" height="7" rx="1.5"/><rect x="3" y="14" width="7" height="7" rx="1.5"/><rect x="14" y="14" width="7" height="7" rx="1.5"/>',
  database: '<ellipse cx="12" cy="5" rx="8" ry="3"/><path d="M4 5v7c0 4 16 4 16 0V5M4 12v7c0 4 16 4 16 0v-7"/>',
  terminal: '<rect x="2" y="4" width="20" height="16" rx="3"/><path d="m6 9 3 3-3 3m6 0h5"/>',
  code: '<path d="m8 5-6 7 6 7m8-14 6 7-6 7m-3-16-2 18"/>',
  "arrow-up-right": '<path d="M6 18 18 6M6 6h12v12"/>',
  lock: '<rect x="5" y="10" width="14" height="11" rx="2"/><path d="M8 10V6a4 4 0 0 1 8 0v4m-4 5v2"/>',
  refresh: '<path d="M20 7a8 8 0 0 0-14-2L3 8m0-5v5h5m-4 9a8 8 0 0 0 14 2l3-3m0 5v-5h-5"/>',
  plus: '<path d="M12 5v14M5 12h14"/>',
  chip: '<rect x="6" y="6" width="12" height="12" rx="2"/><path d="M9 2v4m6-4v4M9 18v4m6-4v4M2 9h4m-4 6h4m12-6h4m-4 6h4"/><rect x="9" y="9" width="6" height="6" rx="1"/>',
  clock: '<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/>',
  activity: '<path d="M2 12h4l3-8 6 16 3-8h4"/>',
  search: '<circle cx="10" cy="10" r="6.5"/><path d="m15 15 6 6"/>',
  "chevron-left": '<path d="m14 6-6 6 6 6"/>',
  "chevron-right": '<path d="m10 6 6 6-6 6"/>',
  sliders: '<path d="M4 6h5m6 0h5M4 12h10m6 0h1M4 18h2m6 0h8"/><circle cx="12" cy="6" r="3"/><circle cx="17" cy="12" r="3"/><circle cx="9" cy="18" r="3"/>',
  cursor: '<path d="m5 3 15 10-7 1-4 7z"/>',
  check: '<path d="m5 12 4 4L19 6"/>',
  trash: '<path d="M3 6h18M9 6V3h6v3m-10 0 1 15h12l1-15M10 10v7m4-7v7"/>',
  "arrow-right": '<path d="M4 12h16m-6-6 6 6-6 6"/>',
  sparkles: '<path d="m12 3 2.5 6.5L21 12l-6.5 2.5L12 21l-2.5-6.5L3 12l6.5-2.5zm7-2v4m-2-2h4"/>',
  alert: '<path d="m10.3 3.8-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.7-3.2l-8-14a2 2 0 0 0-3.4 0ZM12 9v4m0 4h.01"/>'
};
function icon(name) {
  const node = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  node.setAttribute("viewBox", "0 0 24 24");
  node.setAttribute("aria-hidden", "true");
  node.innerHTML = icons[name] || icons.database;
  return node;
}
document.querySelectorAll("[data-icon]").forEach((node) => node.append(icon(node.dataset.icon)));

const state = { connected: false, rows: [], total: 0, offset: 0, selected: null, mode: null, dirty: false, history: [], historyIndex: 0, refreshing: false, searching: 0, sequence: 0 };
let toastTimer;
function toast(message, error = false) {
  $("#toast-text").textContent = message;
  $("#toast").classList.toggle("error", error);
  $("#toast").hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { $("#toast").hidden = true; }, error ? 6000 : 3500);
}

async function api(path, options = {}) {
  const response = await fetch("/api/" + path, { ...options, headers: { "Content-Type": "application/json", ...options.headers }, cache: "no-store" });
  const result = await response.json();
  if (!response.ok) throw new Error(result.error || "The request could not be completed.");
  return result;
}
function write(path, body) { return api(path, { method: "POST", body: JSON.stringify(body) }); }
function bytes(value) {
  if (value < 1024) return [String(value), "B"];
  if (value < 1024 ** 2) return [(value / 1024).toFixed(1), "KB"];
  return [(value / 1024 ** 2).toFixed(1), "MB"];
}
function metric(selector, value, unit = "") {
  const el = $(selector);
  el.replaceChildren(document.createTextNode(value));
  if (unit) { const small = document.createElement("small"); small.textContent = unit; el.append(small); }
}
function duration(seconds) {
  if (seconds < 60) return [String(Math.floor(seconds)), "sec"];
  if (seconds < 3600) return [String(Math.floor(seconds / 60)), "min"];
  if (seconds < 86400) return [(seconds / 3600).toFixed(1), "hrs"];
  return [(seconds / 86400).toFixed(1), "days"];
}
function ttlLabel(ttl) {
  if (ttl === -1) return "No expiry";
  if (ttl < 0) return "Expired";
  const seconds = Math.ceil(ttl / 1000);
  if (seconds < 60) return seconds + "s";
  if (seconds < 3600) return Math.ceil(seconds / 60) + "m";
  if (seconds < 86400) return Math.ceil(seconds / 3600) + "h";
  return Math.ceil(seconds / 86400) + "d";
}
function connection(connected, error = "") {
  state.connected = connected;
  $("#live-label").classList.toggle("offline", !connected);
  $("#live-label").replaceChildren(document.createElement("i"), document.createTextNode(connected ? "Server connected" : "Disconnected"));
  $("#connection-banner").hidden = connected;
  $("#connection-banner>span:nth-child(2)").textContent = error || "Mini Redis is unavailable. Check your server terminal.";
  $("#add-key").disabled = !connected;
  $("#load-sample").disabled = !connected;
  if (!connected) {
    ["#stat-keys", "#stat-memory", "#stat-uptime", "#stat-latency"].forEach((selector) => metric(selector, "—"));
    $("#stat-uptime-note").textContent = "Waiting for server";
  }
}
async function status() {
  try {
    const data = await api("status");
    connection(true);
    const info = data.info;
    const count = Number((info.db0 || "keys=0").split("=")[1]);
    metric("#stat-keys", count.toLocaleString());
    metric("#stat-memory", ...bytes(Number(info.used_memory || 0)));
    metric("#stat-uptime", ...duration(Number(info.uptime_in_seconds || 0)));
    metric("#stat-latency", data.latency_ms.toFixed(2), "ms");
    $("#version").textContent = "v" + info.mini_redis_version;
    $("#stat-uptime-note").textContent = (info.io_model || "epoll") + " · AOF " + (info.aof_enabled === "1" ? "enabled" : "disabled");
    $("#live-label").title = "Connected to " + data.endpoint;
    $("#nav-key-count").textContent = count;
    $("#last-updated").textContent = "Updated " + new Date().toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
    return true;
  } catch (error) { connection(false, error.message); return false; }
}

function renderKeys() {
  const body = $("#key-rows");
  body.replaceChildren();
  const now = Date.now();
  state.rows.forEach((key) => {
    const row = document.createElement("tr");
    row.classList.toggle("selected", key.id === state.selected?.id);
    const name = document.createElement("td");
    const nameWrap = document.createElement("span"); nameWrap.className = "key-name";
    const glyph = document.createElement("span"); glyph.className = "key-glyph"; glyph.textContent = "Aa";
    const keyButton = document.createElement("button"); keyButton.className = "key-select"; keyButton.textContent = key.key || '""'; keyButton.title = key.key;
    keyButton.addEventListener("click", (event) => { event.stopPropagation(); selectKey(key.id); });
    nameWrap.append(glyph, keyButton); name.append(nameWrap);
    const value = document.createElement("td"); value.textContent = key.preview; value.title = key.preview;
    const ttl = document.createElement("td");
    const badge = document.createElement("span"); badge.className = "ttl-badge" + (key.ttl_ms >= 0 ? " expiring" : "");
    if (key.ttl_ms >= 0) badge.dataset.expires = now + key.ttl_ms;
    badge.textContent = ttlLabel(key.ttl_ms); ttl.append(badge);
    const size = document.createElement("td"); size.textContent = bytes(key.bytes).join(" ");
    const actions = document.createElement("td");
    const remove = document.createElement("button"); remove.className = "row-more"; remove.setAttribute("aria-label", "Delete " + key.key); remove.title = "Delete key"; remove.append(icon("trash"));
    remove.addEventListener("click", (event) => { event.stopPropagation(); confirmDelete(key); }); actions.append(remove);
    row.append(name, value, ttl, size, actions); row.addEventListener("click", () => selectKey(key.id)); body.append(row);
  });
  $("#key-count").textContent = state.total;
  $("#keys-empty").hidden = state.rows.length > 0;
  if (!state.rows.length) {
    const searching = $("#key-search").value.length > 0;
    $("#keys-empty h3").textContent = searching ? "No keys found" : "Your keyspace starts here";
    $("#keys-empty p").textContent = searching ? "Try a different name or clear your filter." : "Add your first key, or load a few examples to see Mini Redis in action.";
    $("#load-sample").hidden = searching;
  }
  $("#results-label").textContent = state.total ? `Showing ${state.offset + 1}–${Math.min(state.offset + 50, state.total)} of ${state.total} keys` : "0 keys in view";
  $("#prev-page").disabled = state.offset === 0;
  $("#next-page").disabled = state.offset + 50 >= state.total;
}
async function loadKeys() {
  const sequence = ++state.sequence;
  try {
    const data = await api(`keys?offset=${state.offset}&search=${encodeURIComponent($("#key-search").value)}`);
    if (sequence !== state.sequence) return;
    if (data.total && state.offset >= data.total) { state.offset = 0; return loadKeys(); }
    state.rows = data.keys; state.total = data.total;
    renderKeys();
  } catch (error) { if (sequence === state.sequence) $("#results-label").textContent = error.message; }
}
async function refresh() {
  if (state.refreshing) return;
  state.refreshing = true;
  try { if (await status()) await loadKeys(); } finally { state.refreshing = false; }
}
function resetInspector() {
  state.selected = null; state.mode = null; state.dirty = false;
  $("#key-form").hidden = true; $("#inspector-empty").hidden = false;
  renderKeys();
}
function fillForm(key = null) {
  state.selected = key; state.mode = key ? "edit" : "create"; state.dirty = false;
  $("#inspector-empty").hidden = true; $("#key-form").hidden = false;
  $("#edit-key").value = key?.key ?? "";
  $("#edit-key").readOnly = Boolean(key);
  $("#edit-value").value = key?.value ?? "";
  $("#edit-ttl").value = key && key.ttl_ms >= 0 ? Math.max(1, Math.ceil(key.ttl_ms / 1000)) : "";
  $("#ttl-note").textContent = key ? "Saving restarts this countdown. Empty means no expiration." : "Leave empty to keep this key indefinitely.";
  const editable = !key || key.editable;
  $("#edit-value").readOnly = !editable;
  $("#edit-ttl").disabled = !editable;
  $("#save-key").disabled = !editable;
  $("#save-key").replaceChildren(icon("check"), document.createTextNode(key ? "Save changes" : "Create key"));
  $("#delete-key").hidden = !key;
  $("#binary-note").hidden = editable;
  $("#format-json").disabled = !editable;
  updateValueSize(); renderKeys();
}
function updateValueSize() { $("#value-size").textContent = new TextEncoder().encode($("#edit-value").value).length.toLocaleString() + " bytes"; }
async function selectKey(id) {
  if (state.dirty && state.selected?.id !== id) { toast("Save or cancel your changes before selecting another key.", true); return; }
  if (state.dirty) return;
  try { fillForm(await api("key?id=" + encodeURIComponent(id))); }
  catch (error) { toast(error.message, true); await loadKeys(); }
}
async function confirmDelete(key) {
  const dialog = $("#delete-dialog");
  $("#delete-description").textContent = `“${key.key}” and its value will be permanently removed.`;
  dialog.returnValue = "cancel";
  dialog.showModal();
  const result = await new Promise((resolve) => dialog.addEventListener("close", () => resolve(dialog.returnValue), { once: true }));
  if (result !== "confirm") return;
  try {
    const data = await api("key?id=" + encodeURIComponent(key.id), { method: "DELETE" });
    if (state.selected?.id === key.id) resetInspector();
    toast(data.deleted ? "Key deleted" : "Key already expired"); await refresh();
  } catch (error) { toast(error.message, true); }
}

// Shell-like quoting, including empty strings and explicit escapes. No eval.
function parseCommand(input) {
  const result = []; let word = "", quote = null, started = false;
  for (let i = 0; i < input.length; i++) {
    const char = input[i];
    if (char === "\\" && quote !== "'") {
      if (++i === input.length) throw new Error("Finish the escape sequence after the backslash.");
      word += ({ n: "\n", r: "\r", t: "\t" })[input[i]] ?? input[i]; started = true;
    } else if (quote) {
      if (char === quote) quote = null; else word += char;
    } else if (char === '"' || char === "'") { quote = char; started = true;
    } else if (/\s/.test(char)) { if (started) { result.push(word); word = ""; started = false; }
    } else { word += char; started = true; }
  }
  if (quote) throw new Error("Close the quoted string before running this command.");
  if (started) result.push(word);
  return result;
}
function replyText(reply, depth = 0) {
  if (reply.type === "null") return "(nil)";
  if (reply.type === "integer") return "(integer) " + reply.value;
  if (reply.type === "error") return "(error) " + reply.value;
  if (reply.type === "array") return reply.value.length ? reply.value.map((item, i) => `${"  ".repeat(depth)}${i + 1}) ${replyText(item, depth + 1)}`).join("\n") : "(empty array)";
  return (reply.binary ? "[binary, UTF-8 preview] " : "") + reply.value;
}
function consoleEntry(command, result, error = false, elapsed = null) {
  const entry = document.createElement("div"); entry.className = "console-entry";
  const line = document.createElement("div"); line.className = "console-command"; line.textContent = command;
  if (elapsed !== null) { const time = document.createElement("span"); time.className = "console-timing"; time.textContent = elapsed.toFixed(2) + " ms"; line.append(time); }
  const output = document.createElement("div"); output.className = "console-result" + (error ? " error" : ""); output.textContent = result;
  entry.append(line, output); $("#console-output").append(entry);
  while ($("#console-output").children.length > 100) $("#console-output").firstElementChild.remove();
  $("#console-output").scrollTop = $("#console-output").scrollHeight;
}
async function runCommand(command) {
  if (!command.trim()) return;
  state.history.push(command); state.historyIndex = state.history.length;
  try {
    const args = parseCommand(command);
    const result = await write("command", { args });
    consoleEntry(command, replyText(result.reply), result.reply.type === "error", result.elapsed_ms);
    await refresh();
  } catch (error) { consoleEntry(command, error.message, true); }
}

$("#refresh").addEventListener("click", refresh);
$("#retry").addEventListener("click", refresh);
$("#key-search").addEventListener("input", () => { clearTimeout(state.searching); state.offset = 0; state.searching = setTimeout(loadKeys, 180); });
$("#prev-page").addEventListener("click", () => { state.offset = Math.max(0, state.offset - 50); loadKeys(); });
$("#next-page").addEventListener("click", () => { state.offset += 50; loadKeys(); });
$("#add-key").addEventListener("click", () => {
  if (state.dirty) { toast("Save or cancel your current changes first.", true); return; }
  fillForm(); $("#edit-key").focus(); $(".inspector").scrollIntoView({ block: "nearest" });
});
$("#cancel-edit").addEventListener("click", resetInspector);
$("#delete-key").addEventListener("click", () => state.selected && confirmDelete(state.selected));
$("#key-form").addEventListener("input", () => { state.dirty = true; updateValueSize(); });
$("#format-json").addEventListener("click", () => {
  try { $("#edit-value").value = JSON.stringify(JSON.parse($("#edit-value").value), null, 2); state.dirty = true; updateValueSize(); }
  catch { toast("This value is not valid JSON.", true); }
});
$("#key-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  const button = $("#save-key"); button.disabled = true;
  try {
    const ttl = $("#edit-ttl").value;
    const result = await write("key", { key: $("#edit-key").value, value: $("#edit-value").value, ttl_seconds: ttl === "" ? null : Number(ttl), create_only: state.mode === "create", edit_only: state.mode === "edit" });
    toast(state.mode === "create" ? "Key created" : "Changes saved"); state.dirty = false;
    await selectKey(result.id); await refresh();
  } catch (error) { toast(error.message, true); }
  finally { button.disabled = false; }
});
$("#load-sample").addEventListener("click", async () => {
  const button = $("#load-sample"); button.disabled = true;
  try {
    for (const item of [ { key: "demo:greeting", value: "Hello from Mini Redis 👋" }, { key: "demo:visits", value: "42" }, { key: "demo:profile", value: '{"name":"Rishabh","project":"Mini Redis","language":"C++17"}' }, { key: "demo:session", value: "This key expires in 5 minutes", ttl_seconds: 300 } ]) {
      try { await write("key", { ...item, create_only: true }); } catch (error) { if (!error.message.includes("already exists")) throw error; }
    }
    toast("Sample keys are ready to explore"); await refresh();
  } catch (error) { toast(error.message, true); }
  finally { button.disabled = false; }
});
$("#command-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  const command = $("#command").value; $("#command").value = ""; await runCommand(command); $("#command").focus();
});
$("#command").addEventListener("keydown", (event) => {
  if (!["ArrowUp", "ArrowDown"].includes(event.key) || !state.history.length) return;
  event.preventDefault(); state.historyIndex = Math.max(0, Math.min(state.history.length, state.historyIndex + (event.key === "ArrowUp" ? -1 : 1)));
  $("#command").value = state.history[state.historyIndex] || "";
});
document.querySelectorAll("[data-command]").forEach((button) => button.addEventListener("click", () => { $("#command").value = button.dataset.command; $("#command").focus(); }));
$("#clear-console").addEventListener("click", () => { $("#console-output").replaceChildren(); });
document.querySelectorAll("[data-section]").forEach((button) => button.addEventListener("click", () => {
  document.querySelectorAll("[data-section]").forEach((item) => item.classList.toggle("active", item === button));
  $("#" + button.dataset.section).scrollIntoView({ block: "start" });
  if (button.dataset.section === "console") $("#command").focus({ preventScroll: true });
}));
document.addEventListener("keydown", (event) => {
  if (event.key === "/" && !["INPUT", "TEXTAREA"].includes(document.activeElement.tagName) && !$("#delete-dialog").open) { event.preventDefault(); $("#key-search").focus(); }
});
setInterval(() => {
  document.querySelectorAll("[data-expires]").forEach((node) => {
    const ttl = Number(node.dataset.expires) - Date.now(); node.textContent = ttlLabel(Math.max(-2, ttl)); node.classList.toggle("expired", ttl <= 0);
  });
}, 1000);
setInterval(() => { if (!document.hidden) refresh(); }, 3000);
document.addEventListener("visibilitychange", () => { if (!document.hidden) refresh(); });
refresh();
