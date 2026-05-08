# ON8CIT WebRadio &mdash; Documentation

Detailed reference docs for the ON8CIT WebRadio firmware. Start with the
top-level [README](../README.md) for a quick overview, then dive in here.

## Contents

| Doc                                  | What's in it |
| ------------------------------------ | ------------ |
| [User guide](user-guide.md)          | Buttons, gestures, on-device screens, web pages, IMU behaviour, captive portals, battery / sleep |
| [CLI reference](cli.md)              | Every command on the USB serial CLI (`help`, `wifi`, `station`, `find`, `captive`, ...) |
| [HTTP API](api.md)                   | Endpoints exposed by the on-device web server (the `/api/*` routes) |
| [Hardware](hardware.md)              | Board variants, pin map, build envs (incl. the rotated-panel variant) |
| [Architecture](architecture.md)      | Module map, threading, NVS schema, source-file responsibilities |
| [Layers & innovation](innovation.md) | What's genuinely new in this build; how Claude.ai was used as a co-worker; the hackathon overnight-batch work plan |
| [Building](building.md)              | PlatformIO setup, build flags, ccache, common pitfalls |

## Conventions

- "Left / Mid / Right" buttons refer to the three GPIO buttons on the
  Waveshare board (GPIO 0, 5, 4 respectively).
- "AP portal" = the SoftAP web page reachable on
  `http://192.168.4.1` or `http://on8cit-setup.local` when the radio
  is in setup mode or when a captive portal blocks normal traffic.
- "Web UI" = the home-network web page reachable at the device's STA
  IP or `http://WebRadio.local` once associated.
- "CLI" = the USB-CDC serial command line at 9600 8N1 (the baud rate
  is virtual on USB).
