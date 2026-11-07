// The Comet mark on a canvas, drawn and animated like the device's boot then idle scenes
// (src/ui-touch/CometMark.cpp). Geometry comes from comet-geom.json, which
// scripts/gen_comet_mark.py writes alongside the firmware's headers.
(function () {
  "use strict";
  var canvas = document.getElementById("comet");
  if (!canvas || !canvas.getContext) return;
  var ctx = canvas.getContext("2d");

  var INK = [151, 196, 176];      // the unlit mark on the night sky
  var ACCENT = [43, 255, 136];    // comet green
  var WHITE = [255, 255, 255];

  // timeline, ms (as on the device)
  var DIM_FROM = 180, DIM_TO = 560, HOP_FROM = 620, ARRIVE = 1500, EMBER = 0.28;
  var IDLE_FROM = ARRIVE + 2600;  // after the boot pulse settles, the idle loop takes over
  var IDLE_RUN = 5200, IDLE_HOP = 1100, IDLE_WAVE = 2600;
  var HOP_W = [1.6, 1.4, 1.2, 1.05, 0.9, 0.78, 0.66, 0.56];

  var reduce = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  var G = null, glow = null, scale = 1, ox = 0, oy = 0, dpr = 1, t0 = 0, visible = true;

  function clamp01(v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
  function easeOut(v) { v = clamp01(v); return 1 - (1 - v) * (1 - v); }
  function mix(a, b, f) { return [a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f]; }
  function rgba(c, o) { return "rgba(" + (c[0] | 0) + "," + (c[1] | 0) + "," + (c[2] | 0) + "," + clamp01(o).toFixed(3) + ")"; }
  function X(x) { return ox + x * scale; }
  function Y(y) { return oy + y * scale; }
  function distToHead(n) { return Math.hypot(n[0] - G.head[0], n[1] - G.head[1]); }

  function disc(x, y, r, col, o) {
    if (o <= 0.004) return;
    ctx.beginPath(); ctx.arc(x, y, Math.max(0.5, r), 0, 2 * Math.PI);
    ctx.fillStyle = rgba(col, o); ctx.fill();
  }
  function ring(x, y, r, w, col, o) {
    if (o <= 0.004 || r < 1) return;
    ctx.beginPath(); ctx.arc(x, y, r, 0, 2 * Math.PI);
    ctx.lineWidth = w; ctx.strokeStyle = rgba(col, o); ctx.stroke();
  }
  function line(x0, y0, x1, y1, w, col, o) {
    if (o <= 0.004) return;
    ctx.beginPath(); ctx.moveTo(x0, y0); ctx.lineTo(x1, y1);
    ctx.lineWidth = w; ctx.lineCap = "round"; ctx.strokeStyle = rgba(col, o); ctx.stroke();
  }

  // the tail's soft light: the spine as a band, blurred once into an offscreen canvas
  function buildGlow() {
    var c = document.createElement("canvas");
    c.width = canvas.width; c.height = canvas.height;
    var g = c.getContext("2d");
    g.filter = "blur(" + Math.round(9 * scale) + "px)";
    for (var i = 0; i < G.spine.length; i++) {
      var s = G.spine[i];
      g.beginPath(); g.arc(X(s[0]), Y(s[1]), Math.max(1, s[2] * scale), 0, 2 * Math.PI);
      g.fillStyle = "rgba(255,255,255," + (0.5 * s[3]).toFixed(3) + ")"; g.fill();
    }
    glow = c;
  }
  function hopAt(f) {
    var path = G.path, hops = path.length, total = 0, i;
    for (i = 0; i < hops; i++) total += HOP_W[Math.min(i, 7)];
    var at = clamp01(f) * total;
    i = 0;
    while (i < hops - 1 && at > HOP_W[Math.min(i, 7)]) { at -= HOP_W[Math.min(i, 7)]; i++; }
    var u = clamp01(at / HOP_W[Math.min(i, 7)]);
    var a = G.nodes[path[i]];
    var b = i + 1 < hops ? G.nodes[path[i + 1]] : G.head;
    var e = 1 - (1 - u) * (1 - u);
    return { x: a[0] + (b[0] - a[0]) * e, y: a[1] + (b[1] - a[1]) * e, px: a[0], py: a[1] };
  }

  function resize() {
    if (!G) return;
    var r = canvas.getBoundingClientRect();
    dpr = Math.min(window.devicePixelRatio || 1, 2);
    canvas.width = Math.max(1, Math.round(r.width * dpr));
    canvas.height = Math.max(1, Math.round(r.height * dpr));
    // fit the mark's box, leaving room on the right and around for the waves and shockwave
    var bw = G.box[0], bh = G.box[1];
    scale = Math.min(canvas.width / (bw * 1.12), canvas.height / (bh * 1.25));
    ox = (canvas.width - bw * scale) / 2 - bw * scale * 0.02;
    oy = (canvas.height - bh * scale) / 2;
    buildGlow();
  }

  function frame(t) {
    var W = canvas.width, H = canvas.height;
    ctx.clearRect(0, 0, W, H);
    var boot = t < IDLE_FROM;
    var D = 1e9, msg = null, ember = 1, litBase = 0, arrive = -1, wavePhase = -1, idle = false;
    if (reduce) {
      D = -1; arrive = 3000; litBase = 1;   // fully lit, still
    } else if (boot) {
      ember = 1 - (1 - EMBER) * easeOut((t - DIM_FROM) / (DIM_TO - DIM_FROM));
      litBase = 1;
      if (t >= HOP_FROM && t < ARRIVE) {
        msg = hopAt((t - HOP_FROM) / (ARRIVE - HOP_FROM));
        D = Math.hypot(msg.x - G.head[0], msg.y - G.head[1]);
      } else if (t >= ARRIVE) {
        D = -1; arrive = t - ARRIVE;
        wavePhase = arrive < 900 ? arrive : (arrive - 900) % 1700;
      }
    } else {
      // idle, but the mark stays lit (on the site the boot's light is kept)
      idle = true; litBase = 1;
      var ti = t - IDLE_FROM, run = ti % IDLE_RUN;
      if (run < IDLE_HOP) {
        msg = hopAt(run / IDLE_HOP);
        D = Math.hypot(msg.x - G.head[0], msg.y - G.head[1]);
      } else { D = -1; }
      arrive = run >= IDLE_HOP ? run - IDLE_HOP : 99999;
      wavePhase = ti % IDLE_WAVE;
    }
    var tw = arrive >= 0 ? clamp01(arrive / 600) : 0;

    function litOf(dist) {
      var delta = dist - D, flash = 0;
      if (D > -1 && D < 1e8 && delta > 0 && delta < 28) flash = 1 - delta / 28;
      var lit = delta > 0 ? clamp01(delta / 6) : 0;
      if (idle) lit = 1;   // in idle everything stays lit; only the flash runs along
      return { lit: lit, flash: flash };
    }

    var hx = X(G.head[0]), hy = Y(G.head[1]);

    // tail glow
    if (glow) {
      var tailOn = ember * 0.55 + (arrive >= 0 ? 0.45 : 0);
      var gcol = arrive >= 0 ? ACCENT : INK;
      ctx.save();
      var off = buildTint(gcol);
      ctx.globalAlpha = clamp01(0.55 * tailOn);
      ctx.drawImage(off, 0, 0);
      ctx.restore();
    }

    var lw = Math.max(1, 1.1 * scale);
    // links
    for (var i = 0; i < G.links.length; i++) {
      var a = G.nodes[G.links[i][0]], b = G.nodes[G.links[i][1]];
      var light = (a[3] + b[3]) / 2;
      var la = litOf(distToHead(a)), lb = litOf(distToHead(b));
      var lit = Math.min(la.lit, lb.lit) * litBase, flash = Math.max(la.flash, lb.flash);
      var o = 0.55 * light * (ember * (1 - lit) + lit * 1.1) + 0.5 * flash;
      var col = (lit > 0 || flash > 0) ? mix(ACCENT, WHITE, flash) : INK;
      line(X(a[0]), Y(a[1]), X(b[0]), Y(b[1]), lw, col, o);
    }
    var headLit = arrive >= 0 ? litBase : 0;
    for (i = 0; i < G.headLinks.length; i++) {
      var n = G.nodes[G.headLinks[i]];
      line(hx, hy, X(n[0]), Y(n[1]), Math.max(1, 1.3 * scale), headLit > 0 ? ACCENT : INK,
           0.7 * (ember * (1 - headLit) + headLit));
    }
    // dust
    for (i = 0; i < G.dust.length; i++) {
      var d = G.dust[i];
      var tws = 0.6 + 0.4 * Math.sin(t / 260 + i * 2.1);
      disc(X(d[0]), Y(d[1]), Math.max(0.8, d[2] * scale), tw > 0 ? ACCENT : INK, d[3] * (ember + tw * 0.6 * tws));
    }
    // nodes
    for (i = 0; i < G.nodes.length; i++) {
      var nd = G.nodes[i], L = litOf(distToHead(nd));
      var twinkle = 1 - tw * 0.22 * (0.5 + 0.5 * Math.sin(t / 210 + i * 1.7));
      var on = Math.min(1, 0.35 + nd[3]) * (ember * (1 - L.lit) + L.lit * twinkle) + L.flash;
      var c = L.flash > 0 ? mix(ACCENT, WHITE, L.flash) : (L.lit > 0 ? ACCENT : INK);
      var r = nd[2] * scale * (1 + 0.6 * L.flash);
      if (r >= 1.4) disc(X(nd[0]), Y(nd[1]), r * 1.9 + 0.5, c, on * 0.12);
      disc(X(nd[0]), Y(nd[1]), Math.max(1, r), c, on);
      if (L.flash > 0.55) ring(X(nd[0]), Y(nd[1]), r + (1 - L.flash) * 9 * scale, Math.max(1, dpr), ACCENT, (L.flash - 0.55) * 1.6);
    }
    // the message
    if (msg) {
      var mx = X(msg.x), my = Y(msg.y);
      line(X(msg.x + (msg.px - msg.x) * 0.45), Y(msg.y + (msg.py - msg.y) * 0.45), mx, my, Math.max(1, 1.6 * scale), ACCENT, 0.9);
      disc(mx, my, Math.max(1.5, 2.3 * scale), WHITE, 1);
    }
    // the radio waves
    var half = G.arcHalf * Math.PI / 180;
    for (i = 0; i < G.arcs.length; i++) {
      var ar = G.arcs[i], pulse = 0;
      if (wavePhase >= 0) {
        var dd = wavePhase - 110 * i;
        if (dd > 0 && dd < 520) pulse = dd < 120 ? dd / 120 : 1 - (dd - 120) / 400;
      }
      var steady = ar[2] * (arrive >= 0 ? 1 : ember);
      var acol = arrive >= 0 ? mix(ACCENT, WHITE, 0.67 * pulse) : INK;
      var w = Math.max(1, ar[1] * scale * (1 + 0.35 * pulse));
      ctx.beginPath(); ctx.arc(hx, hy, ar[0] * scale + w / 2, -half, half);
      ctx.lineWidth = w; ctx.lineCap = "round"; ctx.strokeStyle = rgba(acol, steady + pulse * 0.6); ctx.stroke();
    }
    // the head
    var boom = arrive >= 0 && arrive < 420 ? 1 - arrive / 420 : 0;
    if (boot && arrive >= 0 && arrive < 1100) {
      var f = arrive / 1100;
      ring(hx, hy, (7 + 58 * easeOut(f)) * scale, Math.max(1, (2.6 - 1.8 * f) * scale), ACCENT, 0.85 * (1 - f));
      if (f > 0.12) {
        var g2 = (f - 0.12) / 0.88;
        ring(hx, hy, (5 + 44 * easeOut(g2)) * scale, Math.max(1, dpr), WHITE, 0.45 * (1 - g2));
      }
    }
    var litHead = arrive >= 0;
    var breathe = litHead && !reduce ? 1 + 0.08 * Math.sin(t / 190) : 1;
    var gr = (16 + 26 * boom) * scale * breathe;
    var grad = ctx.createRadialGradient(hx, hy, 0, hx, hy, gr);
    var hc = litHead ? ACCENT : INK;
    grad.addColorStop(0, rgba(hc, (litHead ? 0.75 : 0.45) + 0.25 * boom));
    grad.addColorStop(1, rgba(hc, 0));
    ctx.fillStyle = grad; ctx.beginPath(); ctx.arc(hx, hy, gr, 0, 2 * Math.PI); ctx.fill();
    disc(hx, hy, (5.4 + 2.5 * boom) * scale, litHead ? mix(ACCENT, WHITE, 150 / 255) : INK, 1);
    disc(hx, hy, (3.2 + 1.8 * boom) * scale, WHITE, 1);
  }

  var tintCache = { key: "", c: null };
  function buildTint(col) {
    var key = col.join(",") + ":" + canvas.width + "x" + canvas.height;
    if (tintCache.key === key) return tintCache.c;
    var c = document.createElement("canvas");
    c.width = canvas.width; c.height = canvas.height;
    var g = c.getContext("2d");
    g.drawImage(glow, 0, 0);
    g.globalCompositeOperation = "source-in";
    g.fillStyle = rgba(col, 1);
    g.fillRect(0, 0, c.width, c.height);
    tintCache = { key: key, c: c };
    return c;
  }

  function loop(now) {
    if (!t0) t0 = now;
    frame(now - t0);
    if (!reduce && visible) requestAnimationFrame(loop);
  }

  fetch("comet-geom.json").then(function (r) { return r.json(); }).then(function (g) {
    G = g;
    resize();
    window.addEventListener("resize", function () { resize(); tintCache.key = ""; if (reduce) frame(0); });
    if ("IntersectionObserver" in window) {
      new IntersectionObserver(function (es) {
        var was = visible; visible = es[0].isIntersecting;
        if (visible && !was && !reduce) requestAnimationFrame(loop);
      }).observe(canvas);
    }
    requestAnimationFrame(loop);
  }).catch(function () { canvas.hidden = true; });
})();
