# A board layer for Comet

Goal: porting a new device should take a day, not a week. Order agreed: finish the
T-Deck Max on ESP-IDF 5.5 first, then bring the T-LoRa Pager over through this layer,
and only then add new boards.

This is a plan, not a finished refactor. Nothing below has been moved yet.

## Where things are today

Survey of `src/` on 2026-11-07 (preprocessor lines `#if/#elif/#ifdef/#ifndef` that
test a board macro such as `HAS_TDECK_MAX`, `TLORA_PAGER`, `HAS_TANMATSU`):

| | |
|---|---|
| Board conditionals in `src/` | 1582 |
| ... of them in `src/ui-touch/UITask.cpp` (87 000 lines) | 1281 |
| `src/main.cpp` | 55 |
| `src/ui-touch/device_caps.h` | 47 (the capability table itself) |
| `src/helpers/esp32/TouchPrefsStore.cpp` | 37 |
| Everything else | about 160, spread over 20 files |

Most-tested macros: `TLORA_PAGER` 411, `HAS_TDECK_PRO` 252 (the Max defines it too),
`HAS_TANMATSU` 191, `HAS_TDECK_MAX` 147, `HAS_THINKNODE_M9` 117, `HAS_TDISPLAY_P4` 113,
`HAS_TDECK_GT911` 88. Sixteen boards live in `variants/`, most inherited from wadamesh.

What the UITask conditionals guard (rough sort by the code that follows each one):

| Subsystem | Conditionals |
|---|---|
| Display, e-paper refresh, backlight, rotation | ~390 |
| Keyboard, touch, encoder, trackball, navigation | ~380 |
| Audio (I2S, codec, amp, tones, WAV, streams) | ~140 |
| Radio, antenna, modem | ~110 |
| Layout (sizes, fonts, which screens exist) | ~100 |
| Power, battery, charging, sleep | ~50 |
| Storage (SD, SPIFFS, mounting) | ~50 |
| Sensors, GPS | ~20 |

Already good:

- `device_caps.h` gives every board a block of `CAP_*` flags (touch, SD, GPS, sound,
  antenna select, ...), and new UI code is meant to test those, not board names.
- The board classes (`TDeckMaxBoard`, `TLoraPagerBoard`) already own power rails,
  the expander, the fuel gauge, charging, antenna and amp switching.
- The IDF build has one `idf/boards/<board>/board.cmake` per board: name, partitions,
  sources, defines, sdkconfig fragment. That is the right unit; it just has to carry
  more.

What makes a port take a week:

1. **Drivers called directly from UITask.** The Max audio path lived in UITask as
   ~400 lines of I2S and codec code inside `#if defined(HAS_TDECK_MAX)`; the Pager,
   the T-Deck and the Tanmatsu each have their own copy beside it. Same for
   display refresh, keyboard scanning, LEDs.
2. **Board names used as capabilities.** `HAS_TDECK_PRO` means "the e-paper UI" in
   many places, which is why the Max has to pretend to be a Pro.
3. **Variants that borrow from each other's folders.** The Max build includes the Pro
   folder for the display and touch drivers and the Pager folder for the ES8311 codec.
4. **Pin and part facts in three places**: `platformio.ini`, `board.cmake`, and
   `#ifndef PIN_...` fallbacks in UITask.

## The layer

One folder per board, behind a small set of fixed interfaces. UITask and main talk to
the interfaces only.

```
boards/
  common/                 shared drivers, one copy each
    audio/es8311.*        codec
    display/gdeq031t10.*  e-paper panel (today TDeckProDisplay)
    input/tca8418.*       keyboard, cst328.* touch, ...
    power/bq27220.*, sy6970.*, xl9555.*
  tdeck_max/
    board.cmake           sources, defines, sdkconfig, partitions (exists)
    board.h               the pin map and parts, the only place they are written
    board.cpp             begin(), power rails, the Board interface below
    caps.h                its CAP_* block, moved out of device_caps.h
  tlora_pager/
    ...
```

Interfaces (C++ abstract classes or plain function tables, one instance per board):

| Interface | Covers | Today in |
|---|---|---|
| `Board` | begin, power rails, sleep, reboot, battery, charging, antenna | board classes (keep, extend) |
| `AudioOut` | open(rate), write(mono or stereo), close, volume, amp | UITask, one copy per board |
| `Display` | flush, refresh policy (full/partial/fast), backlight, e-paper or colour | variants/*Display.cpp + UITask |
| `Input` | keys, pointer, encoder, into one LVGL indev feed | helpers/input + UITask |
| `Storage` | SD mount, card info, format allowed | UITask + main |
| `Location`, `Imu`, `Haptics` | optional, null when absent | variants |

Capabilities stay `CAP_*`, now taken from the board folder's `caps.h`. Anything a
screen needs to know about the device comes from a `CAP_*` or an interface, never
from a board name. The allow-list of genuine one-off quirks stays short and is
commented where it is used, as `device_caps.h` already asks.

The Max's I2S port (commit "Max audio on the ESP-IDF 5 I2S channel driver") already
put its audio behind a handful of `maxI2s*` functions; that is the shape `AudioOut`
takes.

## Steps

Each step builds and runs on the Max before the next one starts, and each is one PR.

1. **`boards/` skeleton and `board.h` for the Max.** Pin map moves out of
   `board.cmake` defines and UITask fallbacks into one header. No behaviour change.
2. **`AudioOut`.** Move the Max audio (already behind `maxI2s*`) into
   `boards/common/audio` + the Max board; UITask's tones, WAV, Lua and Books paths
   call the interface. Then the Pager's copy goes through the same code.
3. **`caps.h` per board.** Split `device_caps.h` into the board folders. Replace
   `HAS_TDECK_PRO` used as "e-paper UI" with `CAP_EPAPER` (and similar) site by site.
4. **`Display`.** The refresh policy and backlight calls in UITask go behind the
   interface; `TDeckProDisplay` becomes `boards/common/display/gdeq031t10`.
5. **`Input`.** One indev feed; keyboard, touch and encoder drivers in `common/input`.
6. **`Storage`, `Board` additions.** SD mounting and power paths out of main and UITask.
7. **The Pager on IDF 5.5**, as `boards/tlora_pager/`, using only the interfaces.
   Whatever it still needs from UITask is a gap in the layer, fixed there.
8. **A porting guide** (`boards/README.md`): copy a board folder, fill in `board.h`,
   `caps.h`, `board.cmake`, pick drivers from `common/`, and what to test.

Boards nobody is running Comet on (CrowPanels, Tanmatsu, T-Display P4, M9, Heltec,
RAK, Wio, SenseCAP, Attaky) are not ported in this round. They keep building on
PlatformIO from the old code until they are either moved over or dropped; that
decision is Josh's.

## Measures of done

- A new board is a new folder plus at most a handful of lines elsewhere.
- No board macro in UITask outside the commented quirk allow-list.
- One copy of each driver.
- The Max and the Pager both build from `idf/` with `idf/build.sh <board>`.

## Risks

- UITask is large and shared by every board: moving code out of it can silently
  change a board nobody tests. Each step therefore keeps the old boards compiling
  (CI) and is tried on the Max, and from step 7 on the Pager.
- The PlatformIO build stays the reference for the boards not yet moved, so the
  interfaces must compile on Arduino 2 / IDF 4.4 as well until those boards are
  moved or dropped.
