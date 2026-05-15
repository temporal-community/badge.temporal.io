# Community Apps

Installable MicroPython apps live under `apps/<app>/` in this directory. These
apps are published to `registry/community_apps.json`, but they are not baked
into the factory filesystem.

The badge installs each app into `/apps/<app>/` from the Community Apps screen.
Use this area for community contributions, larger examples, and apps that
should stay outside the default home menu.

The generated registry also contains some preloaded factory apps from
`firmware/initial_filesystem/apps/`. Those entries are already on the badge
after a factory flash and are adopted as installed; the apps in this directory
are the ones that are actually downloaded as optional community submissions.

## Current Community Contributions

No community app submissions are included in the initial public release commit.
Submissions should land as separate changes so their authorship and licensing
are easy to audit.

## Adding an App

Create a folder with a `main.py` entry point:

```text
apps/my_app/
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
