# Pager UI

The T-LoRa Pager gets its own interface: built for the scroll wheel and the
keyboard, with no touch layer, in Comet's night-sky look. Mockups are in the
"Comet Pager UI" design canvas. Roadmap issue: #14.

## Rules

1. **Nothing gets lost.** Every function of today's UI stays reachable on the
   Pager. A screen is only replaced when its new version does everything the
   old one did.
2. **Wheel grammar, the same everywhere:**
   - turn = move the focus;
   - press = open or confirm;
   - Back = up one level;
   - long press = the item's actions.

   The keyboard types directly into the focused screen, and letters jump in
   lists. A hint line at the bottom always shows what the keys do right now.
3. **One bright thing.** The comet colour marks the focus (the comet head: a dot
   with a short tail, about 120 ms slide). Everything else is quiet.
4. **The user decides the look** (see Customising).

## How we get there without losing anything

The deep logic stays: map, repeater admin, radio, Wi-Fi, apps, Books, backups
and so on. The rework happens in three layers:

| Layer | What | Result |
|---|---|---|
| A. Theme | Night sky replaces the aurora; comet-head focus style; Pager fonts and sizes | Every existing screen looks like Comet at once |
| B. Wheel | Every existing screen is fully usable with the wheel and keys (keypad nav groups exist, `NavSpatial.h`); no step needs touch | No dead ends |
| C. New screens | Home, Inbox, Chat, Mesh, Settings frame and value editor drawn fresh | The main flows are Pager-native |

Deep pages are then redrawn one by one inside the new frame. Each one comes
with a checklist of its old functions before the old version goes.

## Function inventory (top level)

| Today | New home |
|---|---|
| Command centre / launcher | Home orbit (apps on an arc, preview line) |
| Chats, channels, mentions, hashtag chats | Inbox (list + preview), Chat |
| Message actions (reply, react, info, path, trace, copy, link/QR) | Long press on a message |
| Quick replies, emoji, special characters | Chat: key shortcuts + picker sheet |
| Contacts, discovered, adverts, sort/filter/search | Nodes app (list + detail pane) |
| Contact actions (DM, admin, telemetry, LOS, nickname, block, share) | Long press on a node |
| Add channel (private, join, hashtag), share contact/channel | Inbox: long press / actions |
| Map, markers, route HUD, map options, offline tiles | Map app (pan with the wheel, zoom with keys) |
| Repeater admin, room join/login, admin console | Opened from a node, keeps its logic |
| Control centre, power menu | Long press on Home, or a key |
| Apps: Books, Snake, Airtime, Monitor, Spectrum, Discover, VNC, Remote, Reader, Terminal, Files, Signal, MQTT, File transfer, USB files, Lua apps and store | Apps sheet; each gets the theme and full wheel use |
| Settings: Device, Profile, Radio & Mesh, Contacts, Wi-Fi, Bluetooth, GPS, Clock, Battery, Sensors, Display, Theme, Keyboard, Sound, Quick replies, Lock screen, General, Backups, Language, MQTT, App permissions | Settings frame (list + value pane), same categories |
| Mesh selection (new) | Mesh screen: MeshCore now, Meshtastic and Alternate later |

## Customising (Settings > Theme)

Already there and kept:
- mode;
- accent colour, more colours, rainbow;
- chat looks;
- background (picture or colour);
- hidden app tiles (`app_hide`).

New or changed on the Pager:
- **Comet colour**: presets (Comet green, Ice blue, Nebula violet, Sun gold,
  Mars red, Polar white), any hue in between by turning, or a hex value by typing.
  It drives focus, badges, the progress ring and the status dot.
- **Sky**: night sky (default, with few or many stars), plain colour, picture,
  or black. The aurora is not offered on the Pager.
- **Comet tail**: on or off; off means no motion at all.
- **Text size**: small, normal, large.
- **Home apps**: which apps sit on the orbit, and in what order.
- **Hint line**: always, for a few seconds, or never.
- **High contrast**: kept and respected by everything above.

## Order of work

1. Layer A: night-sky background and colour tokens; comet colour setting with
   presets and hue.
2. Layer B: a wheel audit of every screen, then fixes.
3. Home orbit and Inbox.
4. Chat and message actions.
5. Settings frame and value editor, moving the categories over.
6. Mesh screen (MeshCore only until the Meshtastic path exists).
7. Nodes, Map, then the remaining apps, one by one.

The Pager builds with PlatformIO. Builds run only on Josh's go.

## Done so far (Pager simulator; check on the device)

- Night sky background, the default; no aurora on the Pager.
- Comet colours: 6 named presets, a hue slider (wheel: click, turn, click), hex.
- Focus ring in a bright tint of the comet colour instead of amber.
- Home orbit (App layout: Orbit or Grid in the launcher options). Back from an
  app returns to the orbit; Back on the orbit stays.
- Inbox: the list and the focused chat's newest messages side by side.
- Contacts: the list and the focused node's details side by side.
- Mesh screen on the orbit: MeshCore (press: radio settings), Meshtastic and
  Alternate marked "later".
- Screen reader groundwork (`A11y.h`, `-DCOMET_A11Y_TRACE`).

Still open: a hint line for the keys (it needs the tab bar's room, so it waits
for a decision); the settings frame and value editor; the chat look; bringing
the deep pages over to the new layout one at a time.

