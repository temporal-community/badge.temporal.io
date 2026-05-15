import { cp, mkdir, readdir, readFile, rm, writeFile } from "node:fs/promises";
import path from "node:path";

const root = process.cwd();
const sourceDir = path.join(root, "docs");
const outputDir = path.join(root, "docs-dist");

function minifyCss(css) {
  return css
    .replace(/\/\*[\s\S]*?\*\//g, "")
    .replace(/\s+/g, " ")
    .replace(/\s*([{}:;,>~+])\s*/g, "$1")
    .replace(/;}/g, "}")
    .trim();
}

function minifyHtml(html) {
  return html
    .replace(/>\s+</g, "><")
    .replace(/\s+>/g, ">")
    .replace(/\s{2,}/g, " ")
    .trim();
}

function stylesheetLinks(html) {
  return [
    ...html.matchAll(
      /^ {4}<link rel="stylesheet" href="(css\/[^"]+\.css)" \/>$/gm,
    ),
  ].map((match) => match[1]);
}

async function build() {
  await rm(outputDir, { recursive: true, force: true });
  await cp(sourceDir, outputDir, {
    recursive: true,
    filter: (source) => !source.endsWith(`${path.sep}README.md`),
  });

  const outputCssDir = path.join(outputDir, "css");
  const htmlFiles = (await readdir(outputDir)).filter((file) =>
    file.endsWith(".html"),
  );

  for (const file of htmlFiles) {
    const outputHtmlPath = path.join(outputDir, file);
    const sourceHtml = await readFile(path.join(sourceDir, file), "utf8");
    const links = stylesheetLinks(sourceHtml);

    if (links.length === 0) {
      throw new Error(`${file} does not include split CSS links`);
    }

    const css = (
      await Promise.all(
        links.map((href) => readFile(path.join(sourceDir, href), "utf8")),
      )
    ).join("\n");

    const cssName = `${file.replace(/\.html$/, "")}.min.css`;
    await writeFile(path.join(outputCssDir, cssName), `${minifyCss(css)}\n`);

    const rewrittenHtml = sourceHtml.replace(
      /(?:^ {4}<link rel="stylesheet" href="css\/[^"]+\.css" \/>\n?)+/gm,
      `    <link rel="stylesheet" href="css/${cssName}" />\n`,
    );

    await writeFile(outputHtmlPath, `${minifyHtml(rewrittenHtml)}\n`);
  }

  for (const entry of await readdir(outputCssDir)) {
    if (!entry.endsWith(".min.css")) {
      await rm(path.join(outputCssDir, entry));
    }
  }

  await mkdir(outputDir, { recursive: true });
}

await build();
