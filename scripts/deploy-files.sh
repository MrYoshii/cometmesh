#!/usr/bin/env bash
# Publish the USB Files page (deploy/files/) to files.cometmesh.org's web root.
# Nginx vhost: deploy/nginx/files.cometmesh.org.conf.
#
# Usage:
#   COMET_VPS=user@your-vps scripts/deploy-files.sh             # deploy
#   COMET_VPS=user@your-vps scripts/deploy-files.sh --dry-run   # preview only
#
# Optional:
#   COMET_FILES_PATH=/srv/comet/files   # override the web root
#
# The VPS target comes from the environment; never commit it.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/deploy/files/"
DEST="${COMET_VPS:?set COMET_VPS=user@host}"
DEST_PATH="${COMET_FILES_PATH:-/srv/comet/files}"

DRY=""
[ "${1:-}" = "--dry-run" ] && DRY="--dry-run"

rsync -avz $DRY --delete "$SRC" "$DEST:$DEST_PATH/"

if [ -z "$DRY" ]; then
  echo
  echo "deployed deploy/files/ -> $DEST:$DEST_PATH"
fi
