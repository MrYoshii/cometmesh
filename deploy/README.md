# cometmesh.org infrastructure

Distribution stack for comet: a **VPS nginx origin behind Cloudflare**.

```
tiles.cometmesh.org     →CF (HTTP, edge-cached) →  nginx → OpenStreetMap / OpenTopoMap
firmware.cometmesh.org  →CF (cache bins)        →  nginx → /srv/comet/firmware
files.cometmesh.org     →CF (HTTP)              →  nginx → the USB Files page
flasher.cometmesh.org   →CF (HTTPS)             →  301 → cometmesh.org
cometmesh.org           →CF (HTTPS)             →  nginx → /srv/comet/site (home page + roadmap)
```

**Map tile styles.** The default `/{z}/{x}/{y}.jpg` route serves **OpenStreetMap**
(the firmware default). An opt-in **OpenTopoMap** topographic style is served from
`/opentopo/{z}/{x}/{y}.jpg` (explicit OSM alias at `/osm/...`); the device requests
it only when the user enables *Map → Options → Topographic map*. Legal: OpenTopoMap
map tiles are **© OpenTopoMap (CC-BY-SA)** over **© OpenStreetMap contributors
(ODbL) + SRTM** — the touch UI shows that attribution when topo is active, and the
14-day disk cache keeps each tile hitting OpenTopoMap at most once per fortnight
(their tile-usage policy asks for a contactable UA + caching, both of which the
transcode service provides). Deploying the topo routes = update
`tiles.cometmesh.org.conf` + `tile-transcode.py`, then
`systemctl restart comet-tile-transcode && nginx -t && systemctl reload nginx`
and purge the Cloudflare cache for `tiles.cometmesh.org/opentopo/*`.

The firmware fetches **tiles + the update-check over plain HTTP** (on-device HTTPS
isn't viable — mbedTLS needs ~30 KB heap, only ~5 KB is free post-Wi-Fi), so the
tile + firmware hosts must stay reachable over HTTP. Cloudflare provides the edge
cache, HTTPS for the flasher, and hides the origin IP (so no IP lives in this repo
or the firmware).

## 1. VPS (origin)

```bash
sudo apt install nginx
sudo mkdir -p /srv/comet/firmware/releases/TOUCH /srv/comet/site /var/cache/nginx/comet-tiles
sudo cp deploy/nginx/tiles.cometmesh.org.conf    /etc/nginx/sites-available/
sudo cp deploy/nginx/firmware.cometmesh.org.conf /etc/nginx/sites-available/
sudo cp deploy/nginx/flasher.cometmesh.org.conf  /etc/nginx/sites-available/   # cometmesh.org + flasher redirect
sudo ln -s /etc/nginx/sites-available/tiles.cometmesh.org.conf    /etc/nginx/sites-enabled/
sudo ln -s /etc/nginx/sites-available/firmware.cometmesh.org.conf /etc/nginx/sites-enabled/
sudo ln -s /etc/nginx/sites-available/flasher.cometmesh.org.conf  /etc/nginx/sites-enabled/
sudo nginx -t && sudo systemctl reload nginx
```

## 2. Cloudflare

- **DNS:** `A`/`AAAA` records for `tiles`, `firmware`, `files`, `flasher`, `@` → the VPS IP,
  all **Proxied** (orange cloud).
- **SSL/TLS:** mode **Flexible** (CF↔origin HTTP) is enough since the origin is
  HTTP-only. **Do NOT enable "Always Use HTTPS"** on `tiles.` or `firmware.` — the
  firmware needs plain HTTP there.
- **Cache Rules:**
  - `tiles.cometmesh.org/*` → Eligible for cache, Edge TTL ~14d.
  - `firmware.cometmesh.org/releases/*/*.bin` → cache, Edge TTL ~1d.
  - `firmware.cometmesh.org/releases/TOUCH` (the listing) → short TTL (~60s) or
    Bypass, so new releases appear promptly.

## 3. Publishing a release

From a comet checkout (builds both boards, refreshes the listing, rsyncs up):

```bash
COMET_VPS=user@your-vps scripts/release.sh beta_2
```

The boards built with ESP-IDF (today the T-Deck Max) are released separately, and
release.sh leaves them out:

```bash
COMET_VPS=user@your-vps scripts/release-idf.sh beta_1             # beta channel
COMET_VPS=user@your-vps scripts/release-idf.sh beta_1 --promote   # same bins to stable
scripts/release-idf.sh beta_1 --no-push                            # build + stage in out/firmware only
```

The on-device check GETs `http://firmware.cometmesh.org/releases/TOUCH` (or `BETA`
with *About → Get test builds*), finds the highest `beta_<N>` above its own tag, and
installs `…/releases/<TOUCH|BETA>/beta_<N>/comet-<board>.bin`. Comet's tags start
again at `beta_1`; a dev build (`dev-<sha>`) is below every release.

What the device needs from these hosts, all served by the configs above: the update
listings and bins, `apps/` (store and languages, `scripts/deploy-apps.sh`), map tiles
(`tiles.`, with `comet-tile-transcode.service`) and the USB Files page (`files.`,
`scripts/deploy-files.sh`). Reports stay off in the firmware until
`comet-reports.service` runs.

## Done

- **Home page** ✅ — `cometmesh.org` is served from `deploy/site/`: the animated mark
  (drawn from `comet-geom.json`, which `scripts/gen_comet_mark.py` writes with the
  firmware's headers), what Comet does, boards, the **roadmap** and install steps.
  Publish with `COMET_VPS=user@your-vps scripts/deploy-site.sh`.
- **The roadmap** is `deploy/site/roadmap.json`: stages, each with items whose
  `status` is `done`, `now` or `next`, plus an `updated` date. Edit it and run
  `deploy-site.sh`; nothing else needs touching.
- **`flasher.cometmesh.org` 301-redirects to the apex** (see
  `deploy/nginx/flasher.cometmesh.org.conf`). A browser installer comes back with the
  first release.
- **`deploy/flasher/`** is the old wadamesh-era standalone flasher page; no deploy
  script publishes it.

## TODO before public launch

- **Re-enable OTA-over-Wi-Fi** in the firmware (currently it version-checks then
  defers to manual flashing).
- **Flip `comet` repo public** = launch.
- Decide tile-proxy sharing: dedicated `tiles.cometmesh.org` (this config) vs
  reusing the meshcomod proxy.

> Never commit the VPS IP, SSH keys, or `COMET_VPS`. Cloudflare fronts the
> origin; the deploy target is supplied via the environment at publish time.
