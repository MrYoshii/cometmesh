// roadmap.html: the cards are the GitHub issues labelled "roadmap" (lane = "status: …"
// label, closed = done, milestone = release, "area: …" and "board: …" labels), read live
// from the GitHub API. roadmap.json gives the lanes, the repo, the allowed authors and a
// fallback copy of the cards (scripts/deploy-site.sh refreshes it); releases.json the
// latest release; boards.json the board names.
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

  var LANES = ["planned", "next", "doing", "testing"];
  function fromIssue(i, authors) {   // the same mapping as scripts/build/roadmap-snapshot.py
    if (i.pull_request || authors.indexOf(i.user.login) < 0) return null;
    var names = i.labels.map(function (l) { return l.name; }), column;
    if (i.state === "closed") {
      if (i.state_reason === "not_planned") return null;
      column = "done";
    } else {
      var lanes = names.filter(function (n) { return n.indexOf("status: ") === 0; }).map(function (n) { return n.slice(8); });
      column = LANES.filter(function (l) { return lanes.indexOf(l) >= 0; })[0] || "planned";
    }
    var c = {
      number: i.number, url: i.html_url, column: column, title: i.title,
      text: (i.body || "").split("\n---\n")[0].trim(),
      tags: names.filter(function (n) { return n.indexOf("area: ") === 0; }).map(function (n) { return n.slice(6); }),
      boards: names.filter(function (n) { return n.indexOf("board: ") === 0; }).map(function (n) { return n.slice(7); })
    };
    var ms = i.milestone && i.milestone.title;
    if (ms) c[column === "done" || column === "testing" ? "shipped" : "target"] = ms;
    return c;
  }
  // Live issues, or null (offline, rate-limited) so the caller falls back to roadmap.json.
  function liveCards(rm) {
    if (!rm.repo) return Promise.resolve(null);
    return fetch("https://api.github.com/repos/" + rm.repo + "/issues?labels=roadmap&state=all&per_page=100",
                 { headers: { Accept: "application/vnd.github+json" } })
      .then(function (r) { if (!r.ok) throw 0; return r.json(); })
      .then(function (list) {
        var cards = list.map(function (i) { return fromIssue(i, rm.authors || []); }).filter(Boolean);
        return cards.length ? cards.sort(function (a, b) { return a.number - b.number; }) : null;
      })
      .catch(function () { return null; });
  }

  json("roadmap.json").then(function (rm) {
    return Promise.all([rm, liveCards(rm), json("releases.json").catch(function () { return {}; }), json("boards.json").catch(function () { return {}; })]);
  })
  .then(function (res) {
    var rm = res[0], live = res[1], rel = (res[2].releases || []), boards = (res[3].boards || []);
    if (live) rm.cards = live;
    var names = {};
    boards.forEach(function (b) { names[b.id] = b.name; });

    var upd = document.getElementById("roadmap-updated");
    upd.textContent = "Where Comet is and where it is going. " +
      (live ? "Live from the project's issues on GitHub." : rm.updated ? "Updated " + day(rm.updated) + "." : "");
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
      var h3 = el("h3", "card-title");
      if (c.url) { var a = el("a", "", c.title); a.href = c.url; h3.appendChild(a); }
      else h3.textContent = c.title;
      li.appendChild(h3);
      if (c.text) li.appendChild(el("p", "card-text", c.text));
      var meta = el("p", "card-meta");
      (c.tags || []).forEach(function (t) { meta.appendChild(el("span", "chip", TAGS[t] || t)); });
      (c.boards || []).forEach(function (id) { meta.appendChild(el("span", "chip chip-board", names[id] || id)); });
      if (c.number) meta.appendChild(el("span", "chip", "#" + c.number));
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
