#!/usr/bin/env bash
# Publish the cometmesh.org home page (deploy/site/) to the VPS web root.
# Nginx vhost: deploy/nginx/flasher.cometmesh.org.conf (the cometmesh.org server block).
#
# Usage:
#   COMET_VPS=user@your-vps scripts/deploy-site.sh             # deploy
#   COMET_VPS=user@your-vps scripts/deploy-site.sh --dry-run   # preview only
#
# Optional:
#   COMET_SITE_PATH=/srv/comet/site   # override the web root
#
# The roadmap on the page is deploy/site/roadmap.json: edit it, then run this.
# The VPS target comes from the environment; never commit it.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/deploy/site/"
DEST="${COMET_VPS:?set COMET_VPS=user@host}"
DEST_PATH="${COMET_SITE_PATH:-/srv/comet/site}"

# the roadmap must parse, or the page shows an error instead of it
python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$ROOT/deploy/site/roadmap.json" \
  || { echo "deploy/site/roadmap.json is not valid JSON" >&2; exit 1; }

DRY=""
[ "${1:-}" = "--dry-run" ] && DRY="--dry-run"

ssh "$DEST" "mkdir -p '$DEST_PATH'"
rsync -avz $DRY --delete "$SRC" "$DEST:$DEST_PATH/"

if [ -z "$DRY" ]; then
  echo
  echo "deployed deploy/site/ -> $DEST:$DEST_PATH"
  echo "note: Cloudflare edge-caches; purge cometmesh.org if a change does not show."
fi
