// install.html: pick a board (from boards.json, ?board=<id> preselects), find its newest
// build on the firmware host, and point the esp-web-tools button and the esptool line at it.
(function () {
  "use strict";
  var FW = "https://firmware.cometmesh.org";
  var pick = document.getElementById("board-pick");
  if (!pick) return;

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }
  function full(b) { return (b.brand ? b.brand + " " : "") + b.name; }

  // The newest manifest for a board: stable first, then the test channel. null if none.
  function findBuild(id) {
    function get(feed, beta) {
      var url = FW + "/" + feed + "/manifest-" + id + ".json";
      return fetch(url, { cache: "no-cache" })
        .then(function (r) { if (!r.ok) throw 0; return r.json(); })
        .then(function (m) { return { url: url, m: m, beta: beta }; });
    }
    return get("latest", false).catch(function () { return get("latest-beta", true); }).catch(function () { return null; });
  }

  var builds = {}, current = null;

  function show(b) {
    current = b.id;
    var info = builds[b.id];
    var installer = document.getElementById("installer");
    var none = document.getElementById("install-none");
    var step = document.getElementById("esptool-step");
    if (info === undefined) { installer.hidden = true; none.hidden = true; step.hidden = true; return; }
    if (!info) {
      installer.hidden = true; step.hidden = true; none.hidden = false;
      none.textContent = "There is no build for the " + full(b) + " yet.";
      return;
    }
    none.hidden = true; installer.hidden = false; step.hidden = false;
    document.getElementById("install-btn").setAttribute("manifest", info.url);
    document.getElementById("install-label").textContent =
      "Install " + info.m.version + (info.beta ? " (test build)" : "") + " on the " + full(b);
    var parts = info.m.builds[0].parts, app = parts[parts.length - 1].path, file = app.split("/").pop();
    var link = document.getElementById("app-link");
    link.href = app; link.textContent = file;
    document.getElementById("esptool-cmd").textContent =
      "esptool --chip esp32s3 --port <PORT> --after watchdog-reset write-flash 0x10000 " + file;
  }

  var want = new URLSearchParams(location.search).get("board");

  fetch("boards.json", { cache: "no-cache" }).then(function (r) { return r.json(); }).then(function (d) {
    var list = d.boards || [];
    var first = list.filter(function (b) { return b.id === want; })[0] || list[0];
    list.forEach(function (b) {
      var lbl = el("label", "pick");
      var input = el("input");
      input.type = "radio"; input.name = "board"; input.value = b.id;
      input.addEventListener("change", function () { show(b); });
      lbl.appendChild(input);
      var t = el("span", "pick-text");
      t.appendChild(el("span", "pick-name", full(b)));
      var state = el("span", "pick-state", "checking for a build");
      t.appendChild(state);
      lbl.appendChild(t);
      pick.appendChild(lbl);
      if (b === first) { input.checked = true; show(b); }
      findBuild(b.id).then(function (info) {
        builds[b.id] = info;
        state.textContent = info ? info.m.version + (info.beta ? ", test build" : "") : "no build yet";
        if (current === b.id) show(b);
      });
    });
  }).catch(function () {
    pick.textContent = "The board list could not be loaded.";
  });
})();
