# Layers & Innovation

How the firmware is layered, what's genuinely new about it, and how
Claude.ai was used as a co-worker to accelerate the build.

This doc deliberately overlaps with
[architecture.md](architecture.md) -- the architecture doc is for
maintainers who need to find a function; this one is for reviewers,
hackathon judges, and anyone deciding "is there anything interesting
going on here?"

---

## 1. Layered architecture

The firmware is structured as five layers, with strict downward
dependencies (a layer only ever calls into layers below it).

```
   ┌──────────────────────────────────────────────────────────┐
   │ 5. UX surfaces                                          │
   │    LCD modes (now-playing, big-clock, sys-info,         │
   │    pickers, captive, station-detail) + web UI + CLI +   │
   │    AP captive portal                                     │
   ├──────────────────────────────────────────────────────────┤
   │ 4. Application logic                                    │
   │    Station list (0..30, NVS-backed, drag-drop ordering) │
   │    Saved-WiFi list (10 slots, promote-on-select)        │
   │    Session state (volume, EQ, last station)             │
   │    Captive-portal probe + auto-resume                   │
   ├──────────────────────────────────────────────────────────┤
   │ 3. Service layer                                        │
   │    audio engine (ESP32-audioI2S + ICY callbacks)        │
   │    net (STA scan/connect, RSSI, mDNS)                   │
   │    discover (Radio-Browser API client)                  │
   │    clock (SNTP + POSIX TZ)                              │
   │    log (serial + SD rotation)                           │
   ├──────────────────────────────────────────────────────────┤
   │ 2. HAL / drivers                                        │
   │    ES8311 codec init                                    │
   │    QMI8658 IMU probe + accel read                       │
   │    ST7789 panel driver (Arduino_GFX) + LovyanGFX sprite │
   │    Buttons (debounced state machine)                    │
   │    Battery ADC + ext0 deep-sleep wake                   │
   │    SD_MMC mount                                         │
   ├──────────────────────────────────────────────────────────┤
   │ 1. Hardware                                             │
   │    Waveshare ESP32-S3 + 1.3" 240x240 ST7789 + ES8311 +  │
   │    optional QMI8658 + 2 MEMS mics + 16 MB flash + 8 MB  │
   │    PSRAM                                                │
   └──────────────────────────────────────────────────────────┘
```

**Why this matters:** every UX surface (LCD picker, web `/wifi`, CLI
`wifi` command, AP `/save`) calls the same application-layer
primitives -- `wifiAddNetwork`, `wifiPromoteNetwork`,
`wifiMoveNetwork`, `stationsAdd`, `stationsMove`, etc. So a "delete
network" or "reorder stations" is one place to fix, not three.

---

## 2. Innovations in this build

This started as a fork of
[VolosR/WaveshareRadioStream](https://github.com/VolosR/WaveshareRadioStream).
The Volos sketch was a single 600-line `.ino` with hardcoded
stations, no WiFi management, and no web UI. Things substantially
new in this build:

### Captive-portal detection ("the easy 90%")
- Raw HTTP/1.1 GET to `connectivitycheck.gstatic.com/generate_204`
  (Android-style); Microsoft fallback.
- Parse the status line + Location header by hand so the captive
  30x is preserved (HTTPClient's auto-follow would mask it).
- New `DM_CAPTIVE` LCD screen surfaces the login URL.
- 15 s background re-probe; auto-resume audio when the portal
  clears.
- SoftAP comes back up while `DM_CAPTIVE` is shown so the user has
  a fallback control surface.

### Multi-surface WiFi management
Four interfaces all backed by the same NVS list:
- LCD WiFi picker (long-press Right inside System Info → 3x3 grid)
- AP captive-portal page (setup mode + during boot connect)
- Web `/wifi` page (saved-list with drag-drop, visible scan,
  add manually)
- Serial CLI (`wifi connect / add / remove / move / scan / clear`)

Every successful user-driven connect promotes the chosen SSID to
slot 0, so the radio "learns" the user's preference for next boot.
The AP portal also lets the user **abort** an in-progress boot
connect attempt mid-flight.

### IMU-driven gestures
- QMI8658 auto-detected at I²C 0x6A / 0x6B; gracefully no-ops if
  absent.
- Face-down on a flat surface for 1 s → audio pauses.
- Any tilt past ~57° off horizontal for 200 ms → audio resumes
  (only if WE paused it -- a manual pause stays).
- Motion event un-dims the backlight without a button press.

### On-device station discovery
The Radio-Browser API client is exposed three ways:
- Web `/discover` page (search by name / genre / country)
- CLI `find <text>` / `find tag <tag>` / `find country <name>` →
  paginated results → `find save <n>` to append.
- Both share the same client code in `discover.cpp`.

### UX details
- Drag-drop reorder for the saved-station list and the
  saved-WiFi list (HTML5 native; on drop, the audio module's
  playing-slot index is fixed up so the stream keeps playing).
- Live HH:MM:SS clock in the web banner: server-seeds the value,
  a 1 Hz `setInterval` ticks it locally so it stays current
  without polling.
- Scroll position preserved across edit / add / delete via
  `sessionStorage` (the 302 redirect-back UX no longer slams the
  user back to the top).
- Shared modal CSS via `[id$=Modal]` attribute selectors so any
  future popover gets the styling for free.

### Build-time variants
- One file (`stations_defaults.h`) to edit for a custom factory
  station list.
- `DISPLAY_ROTATION` build flag → separate PIO env
  (`waveshare-s3-radio-rot90`) for sideways enclosure mountings.
- Pre-build hooks for Windows (path-length workaround for
  esp-matter headers) and ccache (huge wins for incremental
  builds against Arduino_GFX's 75-driver compile-everything
  library).

---

## 3. Co-worker tooling: Claude.ai

Claude.ai was the primary acceleration tool for this build, used as
a pair-programming partner across the whole stack.

### How it was used

| Task type | Claude's role |
| --------- | ------------- |
| Architecture decisions | Brainstormed multi-option tradeoffs (e.g. WiFi management surfaces, captive-portal handling), produced "do option 1, here's why option 2/3 are worse" recommendations. |
| Implementation | Wrote the bulk of new C++ modules (imu, discover, web handlers, captive probe) from focused English prompts. |
| Refactors | Performed multi-file refactors (e.g. stations.cpp going from "compiled defaults + per-slot overrides" to "packed NVS list with add/delete/move") in one pass. |
| Debugging | Diagnosed compile errors (e.g. LovyanGFX `textWidth` deprecation, Arduino_GFX text-size leakage), traced runtime issues (IMU polarity, WiFi insta-wake from deep-sleep). |
| Documentation | Wrote and maintained this `docs/` tree, the `ToDo.txt` running ledger, and code comments. |
| UX | Drafted CSS, HTML structure, drag-drop wiring, modal patterns. |

### What it accelerated

Rough estimate: a build that would have taken **3-4 weeks of evenings
solo** was completed in **~10 days of evenings with Claude**. The
biggest wins were:
- Multi-surface consistency. Asking "now do the same for the AP
  portal" got a near-perfect mirror of the just-written web page,
  so all three surfaces stayed in sync without tedious copy-paste.
- Knowing-when-to-stop. Claude pushes back on over-engineering when
  prompted ("Is this worth doing?"), which kept scope honest.
- Writing the parts I find tedious -- HTTP form parsers, JSON
  builders, CSS for modals. Hours saved on each.
- API discovery. "What's the LovyanGFX equivalent of this Arduino_GFX
  call?" answered instantly with working code.

### What didn't work

- Claude can't see the device. Issues that needed an oscilloscope or
  the actual LCD ("why is this overlapping?") still required me to
  capture screenshots and feed them back. Claude reads images
  competently and usually identifies the bug from a photo.
- Long-running build / runtime tests. Claude can't flash to USB and
  watch serial output, so feedback loops were always
  "describe → patch → flash → describe again".
- The QMI8658 IMU polarity was a guess that needed real-hardware
  verification (the `+Z` axis points out the back of the case, not
  the screen, on this particular Waveshare module variant).

### Collaboration style

- I drove the WHAT; Claude drove the HOW.
- Every Claude-produced commit was reviewed by me before push (most
  had small fixes, a few had to be rewritten when my prompt was
  ambiguous).
- The `winRadio/ToDo.txt` ledger was append-only by Claude on each
  round, so the project had a built-in changelog.

---

## 4. Hackathon overnight-batch plan

The intent for the hackathon is to use Claude as both the daytime
pair-partner and the **overnight batch worker** -- queue a list of
prepared tasks before bed, wake up to PR-ready commits.

### Schedule

- **Friday daytime**: live development with Claude as pair partner,
  same as the run-up to the hackathon. Ideas captured in
  `winRadio/ToDo.txt`.
- **Friday evening**: 1-2 hour pre-screen session (see rubric below)
  to take the day's loose ideas and turn them into a Friday-night
  task pack.
- **Friday night**: queued batch executes unattended.
- **Saturday morning**: review + integrate the overnight commits.
  Revisions / fixups go on Saturday's task list.
- **Saturday daytime**: live integration, demo prep, hardware
  testing.
- **Saturday evening**: pre-screen for the Saturday-night batch
  (smaller, more polish-focused than Friday's).
- **Saturday night**: final batch executes.
- **Sunday morning**: integrate, dry-run the demo, submit.

### Pre-screen rubric

Every task that goes into a batch must pass this checklist, agreed
with Claude before submission:

1. **Self-contained.** Task description includes everything Claude
   needs (file paths, line numbers, expected behaviour). No
   "Claude figures it out" steps.
2. **Reversible.** No destructive operations (no `rm`, no NVS-erase,
   no `git push --force`). Worst case is a commit that gets
   reverted.
3. **Testable from output.** A clear "done when..." condition
   visible from the resulting diff (e.g. "this function exists",
   "this CSS rule is added", "this CLI command appears in the
   help"). No "test it on real hardware" steps -- those wait for
   morning.
4. **Bounded.** Max ~2 hours of agent time per task. Larger items
   are split into a chain.
5. **Independent.** Tasks in the batch don't depend on each other's
   commits (or are explicitly chained with `Agent` followups).
6. **Pre-flighted with Claude.** A 5-minute conversation before
   queueing: "Does this prompt make sense? What are you likely to
   misunderstand? What edge cases am I missing?" Claude's
   clarifications are folded into the task before submission.

### Robustification checklist

For each queued task, we add:
- **Inputs.** Exact file paths, function names, expected before/after.
- **Constraints.** "Don't change the public API of X." "Use existing
  helpers like `htmlEscape()` rather than rolling your own."
- **Negative space.** "If you find yourself rewriting Y, stop and
  flag instead." Stops Claude from going off-piste during
  long-context runs.
- **Verification.** "Print a diff stat at the end." "Run the build
  in a worktree and report any compile errors." (Background-mode
  agents can do this.)
- **Output format.** "Commit with message starting `hackathon: ...`
  so we can spot the batch contributions in `git log`."

### Candidate Friday-night batch (placeholder)

This list is a starting point; the real batch will be assembled
during Friday-evening pre-screen. Each item is sized to fit in the
batch.

- [ ] Add a `/api/state` long-polling endpoint so the home page
      auto-refreshes without browser reloads.
- [ ] Add a "scheduled wake" feature: deep-sleep until N minutes
      from now, then resume the last station. RTC alarm wake.
- [ ] Add station-favicons cache (download to PSRAM on first listen,
      show on the home page).
- [ ] Add OTA support behind a CLI command + web button.
- [ ] Add a "now playing history" log (last 50 songs) to SD with a
      web view.

(See [innovation roadmap](#5-roadmap-after-the-hackathon) below for
post-event follow-ons.)

---

## 5. Roadmap after the hackathon

Items that have been considered and parked:
- Podcast playback (RSS feed parser + episode list).
- Microphone uses (clap-to-pause, ambient auto-volume,
  short air-check recorder).
- Bluetooth Low Energy → AirPlay-style streaming receiver.
- A "second" radio in bridge mode for captive-portal walls
  (NAPT through the radio's SoftAP, see captive-portal discussion
  in user-guide.md).

These are intentionally NOT in the hackathon scope -- the goal is to
ship the core experience cleanly, not to chase shiny new pieces.

---

## Provenance

This document, like every other doc in this folder, was drafted by
Claude.ai (Sonnet/Opus 4.x) from English prompts and reviewed by the
author before commit. Where it makes claims about timings, line
counts, or feature inventories, those numbers are pulled from the
actual git history -- not estimated.
