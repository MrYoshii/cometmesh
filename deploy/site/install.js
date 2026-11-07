// install.html: pick a board (from boards.json, ?board=<id> preselects), find its newest
// build on the firmware host, and flash it from the browser with esptool-js (Web Serial),
// showing the same log a local esptool run prints.
import { ESPLoader, Transport } from "./vendor/esptool-js-0.6.1.js";

const FW = "https://firmware.cometmesh.org";
const $ = (id) => document.getElementById(id);
const pick = $("board-pick");

function el(tag, cls, text) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text != null) e.textContent = text;
  return e;
}
const full = (b) => (b.brand ? b.brand + " " : "") + b.name;
const kb = (n) => (n / 1024).toFixed(n < 10240 ? 1 : 0) + " KB";

// The newest manifest for a board: stable first, then the test channel. null if none.
async function findBuild(id) {
  for (const [feed, beta] of [["latest", false], ["latest-beta", true]]) {
    try {
      const url = `${FW}/${feed}/manifest-${id}.json`;
      const r = await fetch(url, { cache: "no-cache" });
      if (r.ok) return { url, m: await r.json(), beta };
    } catch (e) { /* next */ }
  }
  return null;
}

const builds = {};
let current = null, busy = false;

function show(b) {
  current = b;
  const info = builds[b.id];
  const serial = "serial" in navigator;
  $("install-unsupported").hidden = serial || !info;
  if (info === undefined) { $("installer").hidden = true; $("install-none").hidden = true; $("esptool-step").hidden = true; return; }
  if (!info) {
    $("installer").hidden = true; $("esptool-step").hidden = true; $("install-none").hidden = false;
    $("install-none").textContent = `There is no build for the ${full(b)} yet.`;
    return;
  }
  $("install-none").hidden = true; $("installer").hidden = !serial; $("esptool-step").hidden = false;
  if (!busy) $("install-go").textContent = `Install ${info.m.version}${info.beta ? " (test build)" : ""} on the ${full(b)}`;
  const parts = info.m.builds[0].parts, app = parts[parts.length - 1].path, file = app.split("/").pop();
  $("app-link").href = app; $("app-link").textContent = file;
  $("esptool-cmd").textContent = `esptool --chip esp32s3 --port <PORT> --after watchdog-reset write-flash 0x10000 ${file}`;
}

// ---- the flashing log ----
const log = $("flash-log");
function line(text, cls) {
  const s = el("span", cls || "", text + "\n");
  log.appendChild(s);
  log.scrollTop = log.scrollHeight;
}
let partial = null;   // esptool-js writes progress with write() and no newline
const terminal = {
  clean() { log.textContent = ""; partial = null; },
  writeLine(d) { partial = null; line(d); },
  write(d) { if (!partial) { partial = el("span"); log.appendChild(partial); } partial.textContent += d; log.scrollTop = log.scrollHeight; },
};
function progress(step, pct) {
  if (step) $("flash-step").textContent = step;
  if (pct != null) {
    const p = Math.max(0, Math.min(100, Math.round(pct)));
    $("flash-pct").textContent = p + "%";
    $("flash-fill").style.width = p + "%";
    $("flash-bar").setAttribute("aria-valuenow", String(p));
  }
}

// ESP32-S3 over its own USB port does not reset on RTS, so restart it the way
// `esptool --after watchdog-reset` does: arm the RTC watchdog with a short timeout.
// (Register values as in esptool 5, targets/esp32s3.py.) First clear the "download mode
// forced over USB" bit, or the chip comes back up in the bootloader instead of Comet.
async function watchdogReset(loader) {
  const BASE = 0x60008000, WDTCONFIG0 = BASE + 0x98, WDTCONFIG1 = BASE + 0x9c, WPROTECT = BASE + 0xb0, WKEY = 0x50d83aa1;
  const OPTION1 = 0x6000812c, FORCE_DOWNLOAD_BOOT = 0x1;
  try { await loader.writeReg(OPTION1, 0, FORCE_DOWNLOAD_BOOT); } catch (e) { /* older ROMs */ }
  await loader.writeReg(WPROTECT, WKEY);
  await loader.writeReg(WDTCONFIG1, 2000);
  await loader.writeReg(WDTCONFIG0, (1 << 31) | (5 << 28) | (1 << 8) | 2);
  await loader.writeReg(WPROTECT, 0);
}

async function install() {
  const b = current, info = builds[b && b.id];
  if (!info || busy) return;
  busy = true;
  const go = $("install-go");
  go.disabled = true;
  $("flash").hidden = false;
  terminal.clean();
  progress("Choose the port", 0);
  let transport = null;
  try {
    const port = await navigator.serial.requestPort();
    transport = new Transport(port, false);
    const loader = new ESPLoader({ transport, baudrate: 921600, terminal });
    progress("Connecting", 0);
    line(`Comet ${info.m.version} for the ${full(b)}`, "log-head");
    const chip = await loader.main();
    const want = info.m.builds[0].chipFamily;
    if (!chip.toUpperCase().startsWith(want.toUpperCase())) {
      throw new Error(`This is a ${chip}, the build is for ${want}. Check that you chose the right board.`);
    }

    const parts = info.m.builds[0].parts, files = [];
    let total = 0;
    for (const [i, p] of parts.entries()) {
      progress(`Downloading ${i + 1} of ${parts.length}`, null);
      const r = await fetch(p.path, { cache: "no-cache" });
      if (!r.ok) throw new Error(`Download failed: ${p.path} (${r.status})`);
      const data = new Uint8Array(await r.arrayBuffer());
      line(`Downloaded ${p.path.split("/").pop()} (${kb(data.length)}) for 0x${p.offset.toString(16)}`);
      files.push({ data, address: p.offset });
      total += data.length;
    }

    const erase = $("erase-all").checked;
    if (erase) { progress("Erasing the whole flash", 0); line("Erasing flash (this takes a while)..."); }
    const done = new Array(files.length).fill(0);
    const t0 = performance.now();
    await loader.writeFlash({
      fileArray: files, flashMode: "keep", flashFreq: "keep", flashSize: "keep",
      eraseAll: erase, compress: true,
      reportProgress: (fi, written, size) => {
        done[fi] = written / size * files[fi].data.length;
        const sum = done.reduce((a, x) => a + x, 0);
        const s = (performance.now() - t0) / 1000;
        progress(`Writing ${fi + 1} of ${files.length} · ${kb(sum)} of ${kb(total)} · ${(sum / 1024 / Math.max(s, 0.1)).toFixed(0)} KB/s`, sum / total * 100);
      },
    });
    progress("Restarting", 100);
    line(`Wrote ${kb(total)} in ${((performance.now() - t0) / 1000).toFixed(1)} s.`);
    line("Restarting the board...");
    try { await watchdogReset(loader); } catch (e) { await loader.after("hard_reset"); }
    progress("Done: the board restarts into Comet", 100);
    line("Done. Unplug and replug if the screen stays dark.", "log-ok");
  } catch (e) {
    const msg = e && e.message ? e.message : String(e);
    if (/No port selected|cancel/i.test(msg)) { progress("No port chosen", 0); line("No port chosen. Plug the board in and try again."); }
    else { progress("Install failed", null); line("Error: " + msg, "log-err"); line("Hold BOOT while plugging the board in, then try again.", "log-err"); }
  } finally {
    try { if (transport) await transport.disconnect(); } catch (e) { /* closed */ }
    busy = false;
    go.disabled = false;
    if (current) show(current);
  }
}
$("install-go").addEventListener("click", install);

const want = new URLSearchParams(location.search).get("board");
fetch("boards.json", { cache: "no-cache" }).then((r) => r.json()).then((d) => {
  const list = d.boards || [];
  const first = list.find((b) => b.id === want) || list[0];
  for (const b of list) {
    const lbl = el("label", "pick");
    const input = el("input");
    input.type = "radio"; input.name = "board"; input.value = b.id;
    input.addEventListener("change", () => { if (!busy) show(b); });
    lbl.appendChild(input);
    const t = el("span", "pick-text");
    t.appendChild(el("span", "pick-name", full(b)));
    const state = el("span", "pick-state", "checking for a build");
    t.appendChild(state);
    lbl.appendChild(t);
    pick.appendChild(lbl);
    if (b === first) { input.checked = true; show(b); }
    findBuild(b.id).then((info) => {
      builds[b.id] = info;
      state.textContent = info ? info.m.version + (info.beta ? ", test build" : "") : "no build yet";
      if (current === b) show(b);
    });
  }
}).catch(() => { pick.textContent = "The board list could not be loaded."; });
