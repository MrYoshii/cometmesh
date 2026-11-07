# Accessibility and Easy mode

Two modes are part of the main firmware, on every board where the hardware
allows. Both can be turned on during setup and changed later in
Settings > Accessibility.

## Setup

A new first step, before the name: **How do you want to use Comet?**

| Choice | What it turns on |
|---|---|
| Standard | Today's Comet |
| Easy | Easy mode |
| Read the screen aloud | The screen reader, plus larger text and high contrast as suggested defaults |

On a board with a speaker, the screen reader can be started from the welcome
screen without seeing it: hold the wheel (Pager) or the trackball (T-Deck)
for 3 s, and it says "Screen reader on". The welcome screen says so in large
text and, once speech is possible, speaks it too.

## Settings > Accessibility

A new category containing:
- the screen reader: on/off, rate, pitch, volume, how much it says (name only,
  or name + role + hint), and key echo while typing;
- Easy mode: on/off;
- shortcuts to text size, high contrast and button sounds, so they can be
  found in one place.

## Screen reader

**What it reads:**
- the focused item when focus moves: its name, role (button, switch on/off,
  slider 60 %, list item 3 of 12) and hint;
- toasts and new messages ("New message from Lukas");
- the screen title when a screen opens.

On the Pager this sits on top of the existing wheel navigation: every stop is
already a focus stop, so reading the focus covers the whole UI.

**How it works:**
- A small speech queue on its own task. New focus cancels the current
  utterance, so turning fast stays responsive.
- Each focus stop gets an accessible name. Most come for free from the label
  inside a button or row. Icon-only buttons get a name set next to them;
  missing names are listed by a debug audit.
- Text goes through the UI language (TR), so it speaks the language the UI
  shows.

**Engine: eSpeak NG** (GPLv3, like Comet). It covers over 100 languages and
stays clear at high rates, which experienced screen-reader users prefer. Its
code and data are about 1.4 MB. PicoTTS sounds more natural, but it has only
6 voices and needs about 1.1 MB of RAM; we can add it later as a second voice.

The voice data lives outside the app image, so it doesn't eat the
OTA slots: either in a data partition or on the SD card, loaded only when the
reader is on. Comet would ship English and German first.

**Boards:**
- Pager (ES8311 speaker) and T-Deck Max (I2S audio): yes.
- Boards without a speaker: no reader. Easy mode only.

## Easy mode

For people who want Comet to be simple. It hides choices and never removes a
function from the firmware. Turning it off brings everything back.

- **Home:** only Chats, Contacts, Map and Settings, larger. On the Pager
  the orbit shows only these, at a larger size.
- **Text and touch:** large text and large targets; on touch boards no
  long-press-only actions (every action also gets a visible button).
- **Chats:** big bubbles, a clear Send button, quick replies on top. Hops,
  SNR and paths are hidden.
- **Settings:** a short list (name, sound, screen, Wi-Fi, language,
  Accessibility). "Show all settings" asks for a confirmation first.
- **Safety:** confirms before anything is sent to a channel or deleted.
  Radio settings are locked; changing them needs a confirmation in words.
- **Help:** a calm "what can I do here" hint line on every screen.

## Order of work

The Pager UI comes first (docs/PAGER_UI.md). While it is built, every new screen
is made ready for this: each focus stop has a name, role and hint that
`A11y.h` can describe (see Prepared so far). After the UI:

1. Settings > Accessibility page, prefs, and the setup step (mode choice only).
2. Easy mode (Home set, settings list, chat look, confirmations).
3. eSpeak NG and the audio path on the Pager, then on the Max.
4. Screen reader: speak the focus, roles, toasts and messages; audit the names.
5. Starting the reader from the welcome screen without seeing it.

## Prepared so far

- `src/ui-touch/A11y.h` builds what the reader would say for any LVGL object:
  - its name: an explicit one set with `a11ySetName()`, otherwise the
    labels inside it;
  - its role and state: button, switch on/off, slider %, checkbox, list
    choice, text field;
  - its place: "3 of 12" among its siblings;
  - an optional hint.
- Builds with `-DCOMET_A11Y_TRACE` log `[a11y] ...` on every focus change,
  which is what the reader will speak. Use it to find stops without a good
  name. Normal builds compile none of it in.
- The orbit names its tiles (app name, position, the line under the arc as
  hint).
