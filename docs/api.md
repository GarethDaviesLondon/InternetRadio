# HTTP API

Every control on the home / `/discover` / `/wifi` pages goes through a
small JSON / form-encoded API. Same endpoints are listed live at
`/api-docs` on the device.

All responses are 200 with text/plain `ok` unless noted; errors are
4xx with a short text-plain reason.

## State

### `GET /api/state`

Returns a JSON snapshot of everything the home page renders.

```json
{
  "ssid":      "MyHotspot",
  "rssi":      -54,
  "ip":        "192.168.4.32",
  "host":      "WebRadio.local",
  "battery":   { "volts": 4.05, "level": 11 },
  "station":   3,
  "stations":  12,
  "stationName": "Radio Caroline",
  "icy":       "RADIO CAROLINE",
  "song":      "Pink Floyd - Money",
  "bitrate":   128,
  "volumeRaw": 12,
  "volume":    3,
  "paused":    false,
  "eq":        { "bass": 0, "mid": 0, "treble": 0 },
  "running":   true,
  "infoCount": 412,
  "clock":     { "date": "Tue 7 May 2026", "time": "14:32:08" },
  "timezone":  "GMT0BST,M3.5.0/1,M10.5.0"
}
```

## Playback

| Method | Path | Body | Notes |
| ------ | ---- | ---- | ----- |
| POST | `/api/station` | `n=<0..N-1>` | Select a saved slot (reconnects audio) |
| POST | `/api/next` | _(none)_ | Cycle to next saved slot |
| POST | `/api/prev` | _(none)_ | Cycle to previous saved slot |
| POST | `/api/play` | _(none)_ | Toggle pause / resume |
| POST | `/api/listen` | `url=<stream>` `name=<label>` | Ad-hoc preview play (does not save) |
| POST | `/api/volume` | `raw=<0..21>` OR `v=<1..5>` OR `d=<+1\|-1>` | Set / step volume |
| POST | `/api/eq` | `b=<-40..6>` `m=<-40..6>` `t=<-40..6>` | Three-band tone |

## Stations (saved list, 0..30)

| Method | Path | Body | Notes |
| ------ | ---- | ---- | ----- |
| POST | `/api/station-edit` | `n=<slot>` `name=<...>` `url=<...>` | Edit slot. `n=-1` (or `n>=count`) appends. |
| POST | `/api/station-del` | `n=<slot>` | Remove slot; list shifts down |
| POST | `/api/station-move` | `from=<src>` `to=<dst>` | Reorder; the playing slot's index is fixed up so the stream keeps playing |

## WiFi

| Method | Path | Body | Notes |
| ------ | ---- | ---- | ----- |
| POST | `/api/wifi-connect` | `idx=<saved index>` | Switch to saved network; promote on success. **Blocks ~10 s**. |
| POST | `/api/wifi-connect-adhoc` | `ssid=<...>` `pass=<...>` | Ad-hoc connect; saves + promotes on success |
| POST | `/api/wifi-add` | `ssid=<...>` `pass=<...>` | Save credentials without an immediate connect |
| POST | `/api/wifi-del` | `idx=<n>` | Remove a saved network |
| POST | `/api/wifi-move` | `from=<n>` `to=<n>` | Reorder saved list |
| POST | `/api/wifi-rescan` | _(none)_ | Trigger a fresh visible-network scan |

> **Note:** `wifi-connect` and `wifi-connect-adhoc` tear down the
> existing TCP connection during the join; the browser tab that
> issued the request will see a connection-reset and need to be
> reloaded at the new IP. The on-page modal warns about this.

## Discovery (Radio-Browser)

`GET /discover?q=<text>&tag=<genre>&country=<name>&src=0` renders the
HTML search page. The result rows POST to `/api/listen` (preview) and
`/api/station-edit` (save).

## System

| Method | Path | Body | Notes |
| ------ | ---- | ---- | ----- |
| POST | `/api/timezone` | `tz=<POSIX TZ string>` | Set timezone (banner + LCD clock follow) |
| POST | `/api/reboot` | _(none)_ | Soft reboot ~1.5 s after the response is sent |

## SoftAP-only endpoints

When the radio is in setup mode (or running the captive-portal
fallback), an additional `WebServer` is up on the SoftAP at
`http://192.168.4.1` / `http://on8cit-setup.local`:

| Path | Action |
| ---- | ------ |
| `GET /` | Setup page (scan + saved-networks card + add form) |
| `POST /save` | Save SSID + password; promote; reboot |
| `POST /wifi-del` | Forget a saved network |
| `POST /wifi-up` | Move saved entry up one slot |
| `POST /wifi-down` | Move saved entry down one slot |
| `POST /abort` | Abort the in-progress boot connect attempt |
| `GET /rescan` | Re-scan from AP+STA mode |

The `/abort` endpoint is what makes the AP useful while the radio is
walking the saved list at boot: the boot loop polls
`provisionAbortRequested()` from its abort callback and breaks out so
the now-top entry (after a reorder) gets tried.

## Authentication

There is none. The radio's web UI is intended for use on a trusted
home network. If you expose it to the internet, do that behind a
reverse proxy with auth.
