# Dad Jokes

A tiny networked app for the Temporal Replay Badge. Press the right button
to fetch a random dad joke from `https://dadjokes736.com` over the badge's
built-in HTTPS client; the plain-text body is word-wrapped onto the OLED.

## Requirements

WiFi credentials must be configured on the badge — the app uses
`badge.http_get()`, so the badge must be connected to a network that can
reach `dadjokes736.com` over HTTPS.

## Controls

| Input             | Action               |
| ----------------- | -------------------- |
| Joystick up/down  | Scroll a long joke   |
| `BTN_RIGHT`       | Fetch a joke         |
| `BTN_BACK`        | Quit                 |

On launch the screen prompts `Press > for a joke`; the app does nothing
on its own until you ask for one.

## How It Works

- `fetch()` calls `badge.http_get(URL)` and returns the plain-text response.
  A `{"ok":false,"error":"…"}` body from the C HTTP layer is surfaced as an
  error on the OLED instead of being displayed as a joke.
- `wrap_strict()` splits the joke at word boundaries, never inside a word, so
  long words may overflow rather than be truncated.
- `wait()` polls buttons and the joystick at ~50 ms intervals, redrawing
  only when the scroll position changes.
