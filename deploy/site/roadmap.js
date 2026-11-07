// Renders roadmap.json into the roadmap section. Edit roadmap.json, not this.
(function () {
  "use strict";
  var route = document.getElementById("route");
  if (!route) return;
  var LABEL = { done: "Done", now: "Being built", next: "Planned" };

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text) e.textContent = text;
    return e;
  }

  fetch("roadmap.json", { cache: "no-cache" }).then(function (r) { return r.json(); }).then(function (data) {
    route.textContent = "";
    (data.stages || []).forEach(function (stage) {
      var li = el("li", "stage");
      li.appendChild(el("i", "dot"));
      li.appendChild(el("h3", "", stage.title));
      var items = el("ul", "stage-items");
      (stage.items || []).forEach(function (it) {
        var status = LABEL[it.status] ? it.status : "next";
        var item = el("li", "item item-" + status);
        var dot = el("i", "dot dot-" + status);
        dot.setAttribute("aria-hidden", "true");
        item.appendChild(dot);
        var body = el("div");
        var h = el("h4", "", it.title);
        h.appendChild(el("span", "item-status", " (" + LABEL[status] + ")"));
        body.appendChild(h);
        if (it.text) body.appendChild(el("p", "", it.text));
        item.appendChild(body);
        items.appendChild(item);
      });
      li.appendChild(items);
      route.appendChild(li);
    });
    if (data.updated) {
      var d = new Date(data.updated + "T12:00:00Z");
      var when = isNaN(d) ? data.updated : d.toLocaleDateString("en-GB", { day: "numeric", month: "long", year: "numeric" });
      document.getElementById("roadmap-updated").textContent = "Where Comet is and where it is going. Updated " + when + ".";
    }
  }).catch(function () {
    route.querySelector(".route-empty").textContent = "The roadmap could not be loaded. It is also on GitHub in deploy/site/roadmap.json.";
  });
})();
