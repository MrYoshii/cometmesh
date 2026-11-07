// roadmap.html: the board from roadmap.json (columns and cards), the latest release from
// releases.json, board names from boards.json. scripts/release.sh keeps the first two
// current; edit roadmap.json by hand to move cards.
(function () {
  "use strict";
  var board = document.getElementById("kanban");
  if (!board) return;
  var TAGS = { firmware: "Firmware", ui: "Interface", radio: "Radio", infra: "Servers", docs: "Docs" };

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }
  function day(s) {
    var d = new Date(s + "T12:00:00Z");
    return isNaN(d) ? s : d.toLocaleDateString("en-GB", { day: "numeric", month: "long", year: "numeric" });
  }
  function json(u) { return fetch(u, { cache: "no-cache" }).then(function (r) { if (!r.ok) throw 0; return r.json(); }); }

  Promise.all([json("roadmap.json"), json("releases.json").catch(function () { return {}; }), json("boards.json").catch(function () { return {}; })])
  .then(function (res) {
    var rm = res[0], rel = (res[1].releases || []), boards = (res[2].boards || []);
    var names = {};
    boards.forEach(function (b) { names[b.id] = b.name; });

    if (rm.updated) document.getElementById("roadmap-updated").textContent = "Where Comet is and where it is going. Updated " + day(rm.updated) + ".";
    if (rel.length) {
      var r = rel[0], line = document.getElementById("release-line");
      line.hidden = false;
      line.textContent = "Latest release: " + r.tag + (r.channel === "beta" ? " (test build)" : "") + ", " + day(r.date) +
        (r.boards && r.boards.length ? ", for " + r.boards.map(function (id) { return names[id] || id; }).join(", ") : "") + ".";
    }

    var cols = {};
    board.textContent = "";
    (rm.columns || []).forEach(function (c) {
      var sec = el("section", "lane lane-" + c.id);
      sec.setAttribute("aria-labelledby", "lane-" + c.id);
      var h = el("h2", "lane-title"); h.id = "lane-" + c.id;
      h.appendChild(el("span", "", c.title));
      var n = el("span", "lane-count", "0"); h.appendChild(n);
      sec.appendChild(h);
      var ul = el("ul", "cards");
      sec.appendChild(ul);
      board.appendChild(sec);
      cols[c.id] = { ul: ul, count: n, n: 0 };
    });

    var cards = [];
    (rm.cards || []).forEach(function (c) {
      var col = cols[c.column];
      if (!col) return;
      var li = el("li", "card");
      li.appendChild(el("h3", "card-title", c.title));
      if (c.text) li.appendChild(el("p", "card-text", c.text));
      var meta = el("p", "card-meta");
      (c.tags || []).forEach(function (t) { meta.appendChild(el("span", "chip", TAGS[t] || t)); });
      (c.boards || []).forEach(function (id) { meta.appendChild(el("span", "chip chip-board", names[id] || id)); });
      if (c.shipped) meta.appendChild(el("span", "chip chip-shipped", "in " + c.shipped));
      else if (c.target) meta.appendChild(el("span", "chip", "for " + c.target));
      if (meta.childNodes.length) li.appendChild(meta);
      col.ul.appendChild(li);
      col.n++;
      cards.push({ li: li, card: c, col: col });
    });
    Object.keys(cols).forEach(function (k) { cols[k].count.textContent = cols[k].n; });

    // Filter by board: a card without boards applies to every board.
    var filter = document.getElementById("filter");
    var opts = [{ id: "", name: "All boards" }].concat(boards.map(function (b) { return { id: b.id, name: b.name }; }));
    opts.forEach(function (o, i) {
      var btn = el("button", "chip-btn", o.name);
      btn.type = "button";
      btn.setAttribute("aria-pressed", i === 0 ? "true" : "false");
      btn.addEventListener("click", function () {
        filter.querySelectorAll("button").forEach(function (x) { x.setAttribute("aria-pressed", x === btn ? "true" : "false"); });
        Object.keys(cols).forEach(function (k) { cols[k].n = 0; });
        cards.forEach(function (x) {
          var show = !o.id || !x.card.boards || !x.card.boards.length || x.card.boards.indexOf(o.id) >= 0;
          x.li.hidden = !show;
          if (show) x.col.n++;
        });
        Object.keys(cols).forEach(function (k) { cols[k].count.textContent = cols[k].n; });
      });
      filter.appendChild(btn);
    });
  }).catch(function () {
    board.innerHTML = '<p class="aside">The roadmap could not be loaded.</p>';
  });
})();
