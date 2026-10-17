# Security and privacy notes

The release contains generic firmware and source, not a dump of a configured unit.
The connected development unit may retain its owner’s private settings; they are
separate from the release files. Only an explicit full factory flash sanitizes a
previously used board for transfer to another person.

## Implemented protections

- Wi-Fi passwords are forwarded to the board without being returned in status
  responses, request logs, saved PC settings or distributed files.
- Radar settings are committed as one NVS value, preventing mixed configuration
  fields after an interrupted write. Settings are validated before application.
- The companion binds to 127.0.0.1 only, checks the Host and Origin, and requires
  a random session token for every mutation. It rejects cross-site, oversized,
  non-JSON, duplicate-field and invalid-number requests.
- The board’s web setup requires the protected setup hotspot, an expected Host,
  JSON and a random session token. Requests through the ordinary LAN interface
  are denied. Every board generates its own hotspot key; there is no universal key.
- Content Security Policy pins the page’s script hash. Both servers prohibit
  framing, disable caching and avoid third-party page assets and analytics.
- Aircraft and route downloads verify HTTPS certificates. The software never
  disables certificate verification. Responses, queues and aircraft counts are
  bounded, and HTTP retry/backoff limits are respected.
- Changing location invalidates old network results and tracks. The configured
  centre is used by both the board and PC feed, including date-line wrapping.

## Boundaries

This release has passed the internal checks in the verification report; that is
not a guarantee of freedom from bugs or an independent security certification.

Wi-Fi credentials are stored on the ESP32 in **unencrypted NVS**. Flash encryption,
secure boot, signed updates and tamper protection are not enabled. Someone with
physical access to the board can read its flash or reprogram it. Never give a used
board to a new owner without a full factory erase and reflash.

Setup uses local HTTP: WPA-protected Wi-Fi protects the phone-to-hotspot link;
USB setup uses the computer’s loopback interface and a USB cable. Do not expose
the companion through a tunnel or reverse proxy. Local users/programs with
physical or administrator access are outside the isolation boundary.

The aircraft provider receives the selected query coordinates and the requesting
internet address. Operating-system/browser location services may have their own
privacy policies. Browser geolocation is optional. No Wi-Fi password is sent to
the aircraft provider. Wi-Fi enterprise authentication and 5 GHz-only networks
are not supported by this setup flow.

Keep pinned dependencies and the CA bundle under maintenance before subsequent
production runs. This release does not claim protection against every denial of
service on the setup network, malicious USB hosts, or compromised PCs/routers.
