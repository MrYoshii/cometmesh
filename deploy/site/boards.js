// boards.html: every board from boards.json, grouped by brand, with a support matrix
// (features down, boards across). The same file drives the installer and releases.
(function () {
  "use strict";
  var root = document.getElementById("brands");
  if (!root) return;

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }
  function mark(level, label) {
    var m = el("span", "mark mark-" + level);
    m.setAttribute("aria-hidden", "true");
    var wrap = el("span", "mark-wrap");
    wrap.appendChild(m);
    wrap.appendChild(el("span", "sr", label));
    return wrap;
  }

  fetch("boards.json", { cache: "no-cache" }).then(function (r) { return r.json(); }).then(function (d) {
    var levels = d.levels || {}, statuses = d.statuses || {};
    var key = document.getElementById("levels");
    Object.keys(levels).forEach(function (lv) {
      var row = el("div", "key-row");
      var dt = el("dt"); dt.appendChild(mark(lv, "")); row.appendChild(dt);
      row.appendChild(el("dd", "", levels[lv]));
      key.appendChild(row);
    });

    root.textContent = "";
    var brands = (d.brands || []).slice();
    (d.boards || []).forEach(function (b) {   // brands named only on a board still get a section
      if (!brands.some(function (x) { return x.name === b.brand; })) brands.push({ name: b.brand || "Other" });
    });
    brands.forEach(function (brand) {
      var boards = (d.boards || []).filter(function (b) { return (b.brand || "Other") === brand.name; });
      if (!boards.length) return;
      var sec = el("section", "maker");
      var id = "brand-" + brand.name.toLowerCase().replace(/[^a-z0-9]+/g, "-");
      sec.setAttribute("aria-labelledby", id);
      var h = el("h2", "brand-name"); h.id = id;
      h.textContent = brand.name;
      sec.appendChild(h);
      if (brand.url) {
        var a = el("a", "brand-link", brand.url.replace(/^https?:\/\//, "")); a.href = brand.url;
        sec.appendChild(a);
      }

      var list = el("ul", "board-list");
      boards.forEach(function (b) {
        var li = el("li", "board");
        var top = el("div", "board-top");
        top.appendChild(el("h3", "board-name", b.name));
        top.appendChild(el("span", "pill" + (b.status === "main" ? " pill-main" : ""), (statuses[b.status] || b.status).split(":")[0]));
        li.appendChild(top);
        li.appendChild(el("p", "board-about", b.radio + " radio · " + b.about));
        li.appendChild(el("p", "board-status", statuses[b.status] || ""));
        var go = el("a", "board-install", "Install on the " + b.name);
        go.href = "install.html?board=" + encodeURIComponent(b.id);
        li.appendChild(go);
        list.appendChild(li);
      });
      sec.appendChild(list);

      var box = el("div", "matrix-box");
      var t = el("table", "matrix");
      var cap = el("caption", "sr", "What works on each " + brand.name + " board");
      t.appendChild(cap);
      var thead = el("thead"), hr = el("tr");
      hr.appendChild(el("th", "", "Feature"));
      boards.forEach(function (b) { var th = el("th", "", b.name); th.scope = "col"; hr.appendChild(th); });
      thead.appendChild(hr); t.appendChild(thead);
      var tb = el("tbody");
      (d.features || []).forEach(function (f) {
        var tr = el("tr");
        var th = el("th", "", f.name); th.scope = "row"; tr.appendChild(th);
        boards.forEach(function (b) {
          var lv = (b.support || {})[f.id] || "none";
          var td = el("td");
          td.appendChild(mark(lv, levels[lv] || lv));
          var note = (b.feature_notes || {})[f.id];
          if (note) td.appendChild(el("span", "cell-note", note));
          tr.appendChild(td);
        });
        tb.appendChild(tr);
      });
      t.appendChild(tb);
      box.appendChild(t);
      sec.appendChild(box);
      root.appendChild(sec);
    });
  }).catch(function () {
    root.innerHTML = '<p class="aside">The board list could not be loaded.</p>';
  });
})();
