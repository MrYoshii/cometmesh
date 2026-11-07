// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Every server the firmware talks to on its own, in one place.
//
// All on Comet's own hosts (deploy/README.md), reachable over plain HTTP because the
// device has no TLS to spare for them:
//   firmware.cometmesh.org  updates (releases/), app store and language packs (apps/)
//   tiles.cometmesh.org     map tiles (an nginx cache in front of OSM / OpenTopoMap)
//   files.cometmesh.org     the USB Files web page
//
// The anonymous test reports stay off until the report service runs on the firmware
// host (deploy/report-service.py); set COMET_HAS_REPORT_SERVER to 1 then.

#ifndef COMET_FIRMWARE_HOST
#define COMET_FIRMWARE_HOST "firmware.cometmesh.org"
#endif
#ifndef COMET_TILE_SERVER
#define COMET_TILE_SERVER   "http://tiles.cometmesh.org"
#endif
#ifndef COMET_FILES_HOST
#define COMET_FILES_HOST    "files.cometmesh.org"     // the USB Files web page
#endif
#ifndef COMET_WEB_URL
#define COMET_WEB_URL       "https://github.com/MrYoshii/cometmesh"
#endif
#ifndef COMET_ISSUES_URL
#define COMET_ISSUES_URL    "https://github.com/MrYoshii/cometmesh/issues"
#endif
#ifndef COMET_HAS_UPDATE_SERVER
#define COMET_HAS_UPDATE_SERVER 1
#endif
#ifndef COMET_HAS_REPORT_SERVER
#define COMET_HAS_REPORT_SERVER 0
#endif
