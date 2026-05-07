# User guide

How to drive the radio with the on-device buttons, the IMU, and the
web pages.

## Front panel

Three buttons; each supports short / double / long gestures.

```
   [L]   [M]   [R]
```

| Button | Short                  | Double-click            | Long press (≥ 2 s) |
| ------ | ---------------------- | ----------------------- | ------------------ |
| Left   | Toggle mode (Now-Playing ↔ Big Clock) | Open Station Detail (in NP) / Pause-toggle (in Big Clock) | Deep sleep |
| Mid    | Previous station (in picker) / Next station (home) | Open Station Picker / Pick selected (in picker) | Open Station Picker |
| Right  | Volume up (home) / Next station (in picker) | (none) | Open System Info |

Combos:

- **Left + Right held 3 s** — soft reboot (useful when the radio is
  battery-only and a power-cycle isn't easy).

## Display modes

The LCD has seven modes. The home modes (Now-Playing, Big Clock) can
be cycled with a Left short-press; the others are triggered by
specific gestures and exit on a short press.

### Now Playing (home)

- Top banner: "ON8CIT WebRadio" + battery indicator
- Clock strip below the banner
- Now-playing card: station name + scrolling song title + bitrate
- Station switcher with the current slot highlighted
- Volume bar on the right
- RSSI + bitrate footer

### Big Clock

Full-screen 7-segment HH:MM:SS, date underneath, scrolling song title
below, paused-indicator + small volume bar at the bottom.

Reached with a Left short-press from Now Playing; same gesture toggles
back.

### System Info (long-press Right)

Battery V + level bar, WiFi SSID, signal-strength bars + dBm, IP
address, `<host>.local` hostname.

Exit on any short press.

A **second** long-press on Right while in System Info opens the WiFi
picker.

### Station Picker (long-press Mid)

3×3 grid of station cards, paged. The currently-playing slot is
shaded; the cursor cell is yellow.

| Gesture                                | Action |
| -------------------------------------- | ------ |
| Right short                            | Cursor forward |
| Mid short                              | Cursor backward |
| Mid double-click / Mid long / Right long | Pick the cursor |
| Left short                             | Cancel |

Cursor wraps; bottom-right + advance moves to the next page; the
last page wraps back to slot 0.

### Station Detail (Left double-click in Now Playing)

Full-screen card showing slot number, friendly name, ICY broadcast
name (when the stream sent one), the full stream URL wrapped onto
multiple lines, current bitrate, and the latest song title.

Any short press returns to Now Playing.

### WiFi Picker (long-press Right while in System Info)

3×3 grid of scanned SSIDs. Each card shows the SSID name and a small
4-bar signal-strength indicator.

| Gesture     | Action |
| ----------- | ------ |
| Mid short   | Cursor forward |
| Mid double  | Connect to highlighted SSID |
| Left / Right short | Cancel |

On select: looks up stored credentials; if found, joins with them;
otherwise tries open auth. A "Connecting to <ssid>..." progress
screen appears with growing-dots indicator and elapsed time.

- **Success** → SSID is promoted to the top of the saved-list (so
  next boot tries it first), audio resumes on the new network,
  screen returns to Now Playing.
- **Failure** → 1.5 s error message, back to the picker.

### Captive Portal screen

Shown automatically when the radio joins a network but the network
intercepts HTTP for a login page. Displays the SSID, the captive
URL, and a hint ("Open this URL on a phone connected to this WiFi").

The radio re-probes every 15 s; when the network lets us through,
audio resumes automatically and the screen dismisses.

While this screen is up the SoftAP `ON8CIT-WinRadio-Setup` is
broadcasting -- connect to it from a phone to switch / abort / reorder
networks. Left short dismisses the screen (the background probe keeps
running).

## IMU (motion + orientation)

The QMI8658 6-axis IMU (when present) drives two automatic gestures:

### Motion-wake

Picking up or tapping the unit un-dims the backlight without needing
a button press.

### Face-down pause / pick-up resume

- Lay the radio screen-down on a flat surface for ≥ 1 s → audio
  pauses.
- Tilt the unit by ≥ ~57° (any direction) for ≥ 200 ms → audio
  resumes (only if WE paused it -- a manual pause is not undone).

The thresholds are deliberately asymmetric so accidental pauses are
rare and resume is instant when you pick the unit up.

If the unit doesn't have an IMU (older Waveshare module variant),
neither gesture fires; the rest of the firmware is unaffected.

## Web UI

Once associated to a network, the radio is reachable on the LAN at
its STA IP and at `http://WebRadio.local`. The home page is the main
control surface.

### Banner

Live ticking HH:MM:SS clock on every page. Click the banner to go
home; the house icon top-right is also a Home link.

### Home page (cards, top to bottom)

| Card        | Controls |
| ----------- | -------- |
| Stations    | Drag-drop reorder; per-row Edit (modal) and quick-pick; Add station; Discover link |
| Now playing | Station name, song, bitrate, volume, WiFi SSID + RSSI |
| Controls    | Prev / Play / Pause / Next |
| Volume      | Fine slider (0..21) + 1..5 buckets |
| EQ          | Bass / Mid / Treble (-40..+6 dB) |
| System      | Manage WiFi link, Set timezone... button (modal), Reboot |

### `/discover`

Radio-Browser API search:
- Free-text name search
- Genre/tag dropdown (alphabetical, top 40 by station count)
- Country dropdown (alphabetical, top 240)

Each result has Listen (ad-hoc preview without saving) and a Save
selector that appends to the saved list (or replaces a slot
explicitly).

### `/wifi`

Saved-network management:
- **Saved networks**: drag-drop reorder; per-row Connect (live-switch)
  and Delete; currently-joined slot tagged
- **Visible networks**: scanned APs not in the saved list; Connect
  prompts for a password and saves on success
- **Add manually**: text inputs for hidden SSID
- Rescan button

### `/api-docs`

Live reference for every JSON / form-encoded endpoint on the device.

## First-time setup

If the radio has never associated to a network:

1. Boot. The LCD shows "Setup mode" / the boot splash; the SoftAP
   `ON8CIT-WinRadio-Setup` is broadcasting.
2. Connect a phone or laptop to the AP. Most OSes pop a captive
   portal automatically; if not, open `http://192.168.4.1` or
   `http://on8cit-setup.local`.
3. Pick a network from the scan list, enter the password, Save.
4. The radio reboots and joins the new network. Audio starts on the
   first compiled-default station.

You can drive the same flow from the USB serial CLI -- see
[cli.md](cli.md).

## Battery + sleep

- **Backlight** dims after 60 s of inactivity, restores to full
  brightness on any button press, IMU motion event, or web request.
- **Deep sleep** is triggered by a long-press on the Left button.
  Wake is on Left button (ext0). Audio amp is muted before the rails
  drop to avoid a click.
- Battery voltage and 0..13 level are surfaced in the Now-Playing
  banner, in the System Info screen, and via `/api/state`.
