// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Every server the firmware talks to on its own, in one place.
//
// Comet has no servers of its own yet. Until it has, the map tiles, the Lua app
// catalogue, the language packs and the USB Files page still come from the
// wadamesh hosts these defaulted to; those are compatible and only read.
//
// What must NOT reach the wadamesh servers from a Comet build is switched off:
// the update check and the on-device update (they would offer and install
// wadamesh's own firmware), and the anonymous reports. Set COMET_HAS_UPDATE_SERVER
// and COMET_HAS_REPORT_SERVER to 1 once Comet's firmware host serves those.

#ifndef COMET_FIRMWARE_HOST
#define COMET_FIRMWARE_HOST "firmware.wadamesh.com"   // app catalogue, language packs (and later: updates)
#endif
#ifndef COMET_TILE_SERVER
#define COMET_TILE_SERVER   "http://tiles.wadamesh.com"
#endif
#ifndef COMET_FILES_HOST
#define COMET_FILES_HOST    "files.wadamesh.com"      // the USB Files web page
#endif
#ifndef COMET_WEB_URL
#define COMET_WEB_URL       "https://github.com/MrYoshii/cometmesh"
#endif
#ifndef COMET_ISSUES_URL
#define COMET_ISSUES_URL    "https://github.com/MrYoshii/cometmesh/issues"
#endif
#ifndef COMET_HAS_UPDATE_SERVER
#define COMET_HAS_UPDATE_SERVER 0
#endif
#ifndef COMET_HAS_REPORT_SERVER
#define COMET_HAS_REPORT_SERVER 0
#endif
