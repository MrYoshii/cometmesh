// The boards table and the installer, both from boards.json (the same list the
// release script and the firmware go by). Which boards have a build is asked of the
// firmware host: the installer manifest for a board exists once it has a release.
(function () {
  "use strict";
  var FW = "https://firmware.cometmesh.org";
  var STATUS = { main: "Main target", supported: "Supported", testing: "Testing" };

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }

  // The newest manifest for a board: stable first, then the test channel. null if none.
  function findBuild(id) {
    function get(feed, beta) {
      return fetch(FW + "/" + feed + "/manifest-" + id + ".json", { cache: "no-cache" })
        .then(function (r) { if (!r.ok) throw 0; return r.json(); })
        .then(function (m) { return { url: FW + "/" + feed + "/manifest-" + id + ".json", m: m, beta: beta }; });
    }
    return get("latest", false).catch(function () { return get("latest-beta", true); }).catch(function () { return null; });
  }

  var builds = {};   // id -> build or null, filled as the fetches return
  var current = null;

  function showInstaller(b) {
    current = b.id;
    var info = builds[b.id];
    var btn = document.getElementById("install-btn");
    var none = document.getElementById("install-none");
    var installer = document.getElementById("installer");
    var step = document.getElementById("esptool-step");
    if (info === undefined) {   // still asking
      installer.hidden = true; none.hidden = true; step.hidden = true;
      return;
    }
    if (!info) {
      installer.hidden = true; step.hidden = true; none.hidden = false;
      none.textContent = "There is no build for the " + b.name + " yet.";
      return;
    }
    none.hidden = true; installer.hidden = false; step.hidden = false;
    btn.setAttribute("manifest", info.url);
    document.getElementById("install-label").textContent =
      "Install " + info.m.version + (info.beta ? " (test build)" : "") + " on the " + b.name;
    var parts = info.m.builds[0].parts;
    var app = parts[parts.length - 1].path;
    var link = document.getElementById("app-link");
    link.href = app;
    link.textContent = app.split("/").pop();
    document.getElementById("esptool-cmd").textContent =
      "esptool --chip esp32s3 --port <PORT> --after watchdog-reset write-flash 0x10000 " + app.split("/").pop();
  }

  function pickRow(b) {
    var lbl = el("label", "pick");
    var input = el("input");
    input.type = "radio"; input.name = "board"; input.value = b.id;
    input.addEventListener("change", function () { showInstaller(b); });
    lbl.appendChild(input);
    var t = el("span", "pick-text");
    t.appendChild(el("span", "pick-name", b.name));
    var state = el("span", "pick-state", "checking for a build");
    t.appendChild(state);
    lbl.appendChild(t);
    return { label: lbl, input: input, state: state };
  }

  fetch("boards.json", { cache: "no-cache" }).then(function (r) { return r.json(); }).then(function (data) {
    var list = data.boards || [];
    var rows = document.getElementById("board-rows");
    var pick = document.getElementById("board-pick");
    rows.textContent = "";
    list.forEach(function (b, i) {
      var tr = el("tr");
      tr.appendChild(el("td", "", b.name));
      tr.appendChild(el("td", "", b.radio || ""));
      tr.appendChild(el("td", "col-about", b.about || ""));
      var st = el("td");
      st.appendChild(el("span", "pill" + (b.status === "main" ? " pill-main" : ""), STATUS[b.status] || b.status));
      tr.appendChild(st);
      rows.appendChild(tr);

      var p = pickRow(b);
      pick.appendChild(p.label);
      if (i === 0) { p.input.checked = true; showInstaller(b); }
      findBuild(b.id).then(function (info) {
        builds[b.id] = info;
        p.state.textContent = info ? info.m.version + (info.beta ? ", test build" : "") : "no build yet";
        if (current === b.id) showInstaller(b);
      });
    });
  }).catch(function () {
    document.getElementById("board-rows").innerHTML =
      '<tr><td colspan="4" class="board-empty">The board list could not be loaded.</td></tr>';
  });
})();
