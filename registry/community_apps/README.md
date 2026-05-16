# Community Apps

Installable MicroPython apps live under `<app>/` in this directory. These
apps are published to `../community_apps.json`, but they are not baked into the
factory filesystem.

The badge installs each app into `/apps/<app>/` from the Community Apps screen.
Use this area for community contributions, larger examples, and apps that
should stay outside the default home menu.

## Current Community Contributions

| App | Contributor | Notes |
|---|---|---|
| Tardigotchi | aask42 | Hatch and care for a tiny tardigrade. |
| Durable Snake | Alexandre Roman | Snake game with three retries. |
| Starfield Nametag | Alexandre Roman | Animated starfield with a personalized nametag. |
| Dad Jokes | iandouglas | Fetch a random dad joke. |

## Adding an App

Create a folder with a `main.py` entry point:

```text
my_app/
  main.py
  icon.py
  engine.py
```

Include metadata near the top of `main.py` so installed apps display well:

```python
__title__ = "My App"
__description__ = "Short menu description"
__icon__ = "/apps/my_app/icon.py"
```

Then regenerate the registry from the repo root:

```sh
python3 firmware/scripts/generate_startup_files.py
```

The GitHub release workflow also regenerates and verifies the registry before
building firmware, so pull requests that add app folders will fail CI if the
generated catalog is stale.
