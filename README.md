# Temporal Badge Website

Static GitHub Pages website for the Temporal electronic badge MicroPython
developer guide.

This project is intentionally simple: HTML and CSS only. There is no build step, package manager, framework, JavaScript, or generated asset pipeline.

## Project Structure

```text
.
|-- index.html             # Home page and illustrated badge hero
|-- developer-guide.html   # Local guide mapped from the Jumperless docs
|-- get-started.html       # JumperIDE and first-app setup
|-- hardware.html          # Hardware map and specs
|-- api-reference.html     # Badge MicroPython API summary
|-- apps.html              # App structure and filesystem notes
|-- hacks.html             # Advanced topics, IR, and gotchas
|-- contribute.html        # Source links
|-- styles.css             # Shared styling for all pages
|-- favicon.svg            # Badge-inspired favicon
`-- README.md
```

## Editing Rules For Codex / Claude Code

Keep the project static.

- Do not add JavaScript.
- Do not add a build system.
- Do not add package manager files.
- Do not add framework-specific files.
- Keep shared visual changes in `styles.css`.
- Keep navigation links consistent across all HTML pages.
- Keep external Temporal links pointed at `https://temporal.io/` if any are
  reintroduced.
- Preserve the Space Mono typography and dark Temporal badge aesthetic.
- Use ASCII-only text unless there is a clear reason not to.

## Visual Direction

The site should feel like a Temporal event badge interface:

- Dark background with subtle grid/scanline texture.
- Accent color: `#C4E644`.
- Space Mono font throughout.
- Sharp panels, minimal rounding, terminal-like labels.
- Hero badge illustration should resemble the real badge screen and controls.
- Header uses the Temporal symbol image:

```text
https://images.ctfassets.net/0uuz8ydxyd9p/5kHQtcFCSqkLDH4I6HFmyG/8e75bd66b57696eb0a7866b3cbf8ea30/Temporal_Symbol_light_1_2x.png
```

## Common Tasks

### Change Content

Edit the relevant `.html` file directly. Most repeated content appears in each page header/footer, so update all pages when changing navigation or global labels.

### Change Styling

Edit `styles.css`. The hero badge illustration is also CSS/HTML, not an image.

Important selectors:

- `.hero` controls the first viewport layout.
- `.badge-body` controls the badge hardware shape.
- `.screen` and `.badge-menu-*` control the badge display.
- `.controls`, `.dpad`, and `.dpad-*` control the badge buttons.
- `.app-icon.*` controls app catalog icons.

### Preview Locally

Open `index.html` directly in a browser. A dev server is not required.

### Deploy To GitHub Pages

Use GitHub Pages with:

- Source: `Deploy from a branch`
- Branch: `main`
- Folder: `/root`

GitHub Pages will serve `index.html` automatically.
