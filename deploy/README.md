# cometmesh.org infrastructure

Distribution stack for comet: **one Debian VPS running nginx**, no CDN in front.
Plain HTTP where the device fetches (it has no TLS to spare), HTTPS from Let's
Encrypt where browsers come.

```
tiles.cometmesh.org     HTTP   nginx (14-day disk cache) → tile-transcode → OSM / OpenTopoMap
firmware.cometmesh.org  HTTP   nginx → /srv/comet/firmware   (updates, apps/, langs)
files.cometmesh.org     HTTPS  nginx → /srv/comet/files      (USB Files page; WebSerial needs HTTPS)
cometmesh.org, www.     HTTPS  nginx → /srv/comet/site       (home, boards, roadmap, install)
flasher.cometmesh.org   HTTPS  301 → cometmesh.org
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
and clear `/var/cache/nginx/comet-tiles` if old topo tiles should go.

The firmware fetches **tiles, updates and the store over plain HTTP** (on-device
HTTPS isn't viable: mbedTLS needs ~30 KB heap), so `tiles.` and `firmware.` must
never redirect to HTTPS.

## 1. VPS

Debian, as root. Deploys run as an unprivileged user `comet` (SSH key only) that
owns `/srv/comet`.

```bash
apt install -y nginx rsync git python3-flask python3-pil python3-requests certbot python3-certbot-nginx
git clone https://github.com/MrYoshii/cometmesh /opt/cometmesh

adduser --disabled-password --gecos "Comet deploy" comet
install -d -m 700 -o comet -g comet /home/comet/.ssh     # + authorized_keys (600)

mkdir -p /srv/comet/{site,files,firmware/releases/TOUCH,firmware/releases/BETA,firmware/apps}
echo '[]' > /srv/comet/firmware/releases/TOUCH/index.json
echo '[]' > /srv/comet/firmware/releases/BETA/index.json
chown -R comet: /srv/comet
mkdir -p /var/cache/nginx/comet-tiles && chown www-data: /var/cache/nginx/comet-tiles

cd /opt/cometmesh/deploy/nginx
cp tiles.cometmesh.org.conf firmware.cometmesh.org.conf files.cometmesh.org.conf flasher.cometmesh.org.conf /etc/nginx/sites-available/
for f in tiles firmware files flasher; do ln -sf /etc/nginx/sites-available/$f.cometmesh.org.conf /etc/nginx/sites-enabled/; done
rm -f /etc/nginx/sites-enabled/default
nginx -t && systemctl reload nginx

mkdir -p /opt/comet-tiles && cp /opt/cometmesh/deploy/tile-transcode.py /opt/comet-tiles/
cp /opt/cometmesh/deploy/comet-tile-transcode.service /etc/systemd/system/
systemctl daemon-reload && systemctl enable --now comet-tile-transcode
```

## 2. DNS and TLS

- **DNS:** `A` (and `AAAA`) for `@`, `www`, `tiles`, `firmware`, `files`, `flasher`
  → the VPS.
- **TLS** for the browser hosts only; `--redirect` adds the HTTPS redirect to just
  these server blocks, `tiles.` and `firmware.` stay HTTP:

```bash
certbot --nginx --redirect -m <mail> --agree-tos \
  -d cometmesh.org -d www.cometmesh.org -d files.cometmesh.org -d flasher.cometmesh.org
```

certbot edits the live copies in `/etc/nginx/sites-available/`. When a config in
this repo changes, copy it over and run the same certbot command again: it
re-adds its lines to the fresh file.

Check:

```bash
curl -sI http://tiles.cometmesh.org/5/17/11.jpg | head -1     # 200, no redirect
curl -s  http://firmware.cometmesh.org/releases/TOUCH         # the listing
curl -sI http://cometmesh.org | head -3                       # 301 -> https
```

## 3. Publishing a release

A release always builds every board from one committed source state (the script
refuses uncommitted firmware changes, an existing tag, or a machine that cannot build
all boards). Every board Comet is released for is one entry in `deploy/site/boards.json` (id =
the firmware's `WADA_BOARD_ID`, build = an `idf/boards` dir or a PlatformIO env).
The release script, the site's board table and installer, and the update check all
go by it.

```bash
COMET_VPS=comet01-deploy scripts/release.sh beta_2             # build all boards, beta channel
COMET_VPS=comet01-deploy scripts/release.sh beta_2 --promote   # the same images to stable
scripts/release.sh beta_2 --no-push                            # build + stage only
```

It writes, per channel (`releases/BETA`, stable `releases/TOUCH`):

- `<tag>/comet-<id>.bin` (+ `-bootloader`, `-partitions`, `-otadata`, `-merged`)
- `index-<id>.json`: the tags that have an image for that board. The device's update
  check reads its own (About → Install update; About → Get test builds for BETA), so
  boards are released independently.
- `latest-beta/` or `latest/manifest-<id>.json`: the browser installer on
  cometmesh.org, pointing at the board's newest tag. It writes the four images at their
  offsets, so an install over Comet keeps the settings.

Tags are `beta_<N>`, from `beta_1`; a dev build (`dev-<sha>`) is below every release.

What the device needs from these hosts, all served by the configs above: the update
listings and images, `apps/` (store and languages, `scripts/deploy-apps.sh`), map tiles
(`tiles.`, with `comet-tile-transcode.service`) and the USB Files page (`files.`,
`scripts/deploy-files.sh`). Reports stay off in the firmware until
`comet-reports.service` runs.

## Done

- **The site** ✅ — `cometmesh.org` is served from `deploy/site/` (`scripts/deploy-site.sh`),
  four pages that all read the JSON files beside them:
  - `index.html`: the animated mark (`comet-geom.json`, written by
    `scripts/gen_comet_mark.py`) and what Comet does.
  - `boards.html`: every board by brand with a support matrix, from `boards.json`
    (`levels`, `statuses`, `features`, `brands`, `boards[].support`).
  - `roadmap.html`: a board of lanes. The cards are the GitHub issues with the label
    `roadmap`, read live from the GitHub API: lane = `status: planned|next|doing|testing`,
    closed = done, `area: …` and `board: …` labels, milestone = the release. Only issues
    by the `authors` in `roadmap.json` are shown; that file is the fallback copy,
    refreshed by `scripts/deploy-site.sh`. The latest release comes from `releases.json`.
  - `install.html`: the browser installer; `?board=<id>` preselects a board.
- **The roadmap is edited on GitHub**: a new card is an issue with the labels `roadmap`
  and `status: …`; moving it is changing the `status:` label; done is closing it. Give it
  a milestone `beta_<N>` and `scripts/release.sh` moves it to testing when that beta is
  published and closes it on `--promote` (with `gh` logged in). The page shows changes
  at once; no deploy needed. `releases.json` is written by `release.sh`; commit it.
- **`flasher.cometmesh.org` 301-redirects to the apex** (see
  `deploy/nginx/flasher.cometmesh.org.conf`). A browser installer comes back with the
  first release.
## TODO before public launch

- **Flip `comet` repo public** = launch.
- Decide tile-proxy sharing: dedicated `tiles.cometmesh.org` (this config) vs
  reusing the meshcomod proxy.

> Never commit SSH keys or `COMET_VPS`; the deploy target comes from the
> environment (an `~/.ssh/config` alias such as `comet01-deploy` works).
