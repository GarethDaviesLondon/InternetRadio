# CLI reference

The radio exposes a USB-CDC serial command line on the same USB port
used for flashing. **Baud rate is virtual** on USB-CDC; PuTTY's /
`screen`'s setting is cosmetic. We pass `9600` to `Serial.begin()`
out of habit.

The CLI is always live -- you can change networks, edit stations, or
inspect status while audio plays. Output is CRLF; both CR and LF
line endings are accepted on input.

## Status / system

| Command | Action |
| ------- | ------ |
| `help`, `?` | Show this list |
| `status`, `stat`, `s` | WiFi state, IP, RSSI, station, song, volume, battery |
| `reboot`, `r` | Restart the device |
| `sleep` | Deep-sleep immediately (Left-button wake) |
| `cancel`, `exit` | Exit setup-mode loop and resume boot |
| `reconnect` | Tear down WiFi and walk the saved list again |
| `captive` | One-shot captive-portal probe (`generate_204` style) |

## Stations

| Command | Action |
| ------- | ------ |
| `stations`, `list` | List all saved stations (with the playing slot starred) |
| `station <n>`, `sel <n>` | Select station N (1-based; reconnects audio) |
| `station edit <n>` | Interactive edit of slot N (name + URL) |
| `station add` | Append a new station (interactive name + URL) |
| `station del <n>` | Delete slot N (list shifts down) |
| `station reset-all` | Wipe the NVS list and reseed from compiled defaults |
| `next`, `prev` | Cycle stations |

### `find` (Radio-Browser search)

| Command | Action |
| ------- | ------ |
| `find <text>` | Free-text name search |
| `find tag <genre>` | Search by genre / tag |
| `find country <name>` | Search by country |
| `find list` | Show the last search's results |
| `find play <n>` | Preview hit N (ad-hoc, doesn't save) |
| `find save <n>` | Append hit N to the station list |

Results are cached so `find list` / `find save N` work after a
`find <text>`.

## Volume + EQ

| Command | Action |
| ------- | ------ |
| `volume`, `vol`, `v` | Show current volume bucket (1..5) |
| `volume <n>`, `vol <n>` | Set volume (1..5) |
| `vol+`, `+` | Step up |
| `vol-`, `-` | Step down |

EQ is web-only; the CLI reads the current values via `status`.

## WiFi

The radio supports up to **10** saved networks; the boot loop walks
the list top-to-bottom.

### Reading state

| Command | Action |
| ------- | ------ |
| `wifi list`, `wifi show` | List saved networks (joined one starred) |
| `wifi scan` | Single scan + table of visible APs |
| `wifi scan-hard`, `wifi rescan` | 3-pass scan; catches slow-beacon hotspots |
| `wifi diag` | Dump driver state (mode, MAC, status, last reason code) |

### Adding / connecting

| Command | Action |
| ------- | ------ |
| `wifi`, `wifi add` | Interactive: scan + pick + password prompt + save |
| `wifi add <ssid>` | Same, pre-filled |
| `wifi connect <ssid>` | Look up stored creds and join (or open-auth + prompt on fail). On success, promote SSID to top of saved list. |
| `wifi connect "<ssid>" <pass>` | Ad-hoc connect with explicit password; offers to save on success |

`wifi connect` prints a growing-dots progress indicator. On a
successful join it auto-runs the captive-portal probe and surfaces
the result on the LCD if a portal is detected.

### Editing the list

| Command | Action |
| ------- | ------ |
| `wifi remove <n>` | Remove saved network N (1-based) |
| `wifi move <from> <to>` | Reorder saved networks (1-based) |
| `wifi clear` | Wipe ALL saved networks |

## Time

| Command | Action |
| ------- | ------ |
| `time`, `time show` | Show synced date / time + timezone |
| `time set <posix-tz>` | Set the POSIX TZ (e.g. `GMT0BST,M3.5.0/1,M10.5.0`) |

## SD card

Optional `SD_MMC` mount:

| Command | Action |
| ------- | ------ |
| `sd status` | Mount state, presence of `/stations.csv` and `/theme.ini` |
| `sd ls <path>` | List a directory |
| `sd reload` | Re-read `/stations.csv` and `/theme.ini` |

## Logging

| Command | Action |
| ------- | ------ |
| `log` | Show all log flags |
| `log on` / `log off` | Stream audio-library events to serial |
| `log verbose on` / `off` | Enable `[DEBUG]` lines |
| `log sd on` / `off` | Mirror log to `/log.txt` on SD (rotated) |

## Shortcuts

- **Ctrl+C** — interrupt the current interactive prompt and reset to
  the `radio>` shell prompt.
- **Backspace / Delete** — both work for line editing.
- **Arrow keys / Home / End** — silently absorbed (no history yet).

## First-time setup over CLI

```
radio> wifi
(scan + pick prompt)
SSID: my-network
Password: ********
Saving... done.
radio> reboot
```

After reboot the radio joins the new network and audio starts.
