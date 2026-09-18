#!/usr/bin/env python3
"""Turns a plugin's manual sources into the one HTML file the PDF is made from.

Called by make-manual.sh, which has already generated the two reference
sections out of the plugin itself. This step is the assembly: a small key/value
header off the top of the Markdown, the generated sections substituted into it,
Markdown to HTML, and the result wrapped in the cover page and the suite
stylesheet.

The output is deliberately self-contained -- the stylesheet is inlined and the
logo is embedded as a data URI -- so the HTML is publishable as a web manual on
its own and wkhtmltopdf needs no access to the filesystem to render it.

  manual.py --plugin RainyDay --version 1.5.1 --source docs/manual.md \
            --params params.md --presets presets.md \
            --css shared/tools/manual.css --logo _designs/logo.png \
            --out RainyDay-Manual.html
"""

import argparse
import base64
import datetime
import html
import pathlib
import re
import sys

import markdown

EXTENSIONS = [
    "tables",
    "def_list",
    "fenced_code",
    "md_in_html",
    "attr_list",
    "sane_lists",
    "smarty",
    "toc",
]

EXTENSION_CONFIGS = {
    # The contents page is the toc extension's own, headed by its title rather
    # than by a heading in the source -- a heading there would list itself.
    # Three levels is the whole structure of a manual; deeper ones are
    # parameter modules and would swamp the page.
    "toc": {"title": "Contents", "toc_depth": "1-3"},
}


def split_header(text):
    """Reads the leading '---' delimited key: value block, if there is one."""
    meta = {}
    if not text.startswith("---"):
        return meta, text
    end = text.find("\n---", 3)
    if end < 0:
        return meta, text
    for line in text[3:end].strip().splitlines():
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        meta[key.strip().lower()] = value.strip()
    return meta, text[end + 4 :].lstrip("\n")


def data_uri(path):
    suffix = pathlib.Path(path).suffix.lower()
    mime = {".png": "image/png", ".svg": "image/svg+xml", ".jpg": "image/jpeg"}.get(
        suffix, "application/octet-stream"
    )
    return "data:%s;base64,%s" % (
        mime,
        base64.b64encode(pathlib.Path(path).read_bytes()).decode("ascii"),
    )


def logo_markup(path, plugin):
    """The cover image, or a plain wordmark when no logo file was given."""
    if path and pathlib.Path(path).is_file():
        return '<img src="%s" alt="%s">' % (data_uri(path), html.escape(plugin))
    return '<p class="wordmark">%s</p>' % html.escape(plugin)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plugin", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--source", required=True)
    ap.add_argument("--params", required=True)
    ap.add_argument("--presets", required=True)
    ap.add_argument("--css", required=True)
    # Optional: a collection that has no logo yet still builds a manual, it
    # just gets a wordmark on the cover instead of an image.
    ap.add_argument("--logo", default="")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    source = pathlib.Path(args.source).read_text(encoding="utf-8")
    meta, body = split_header(source)

    substitutions = {
        "{{PARAMETER_REFERENCE}}": pathlib.Path(args.params).read_text(encoding="utf-8"),
        "{{PRESET_LIBRARY}}": pathlib.Path(args.presets).read_text(encoding="utf-8"),
        "{{PLUGIN}}": args.plugin,
        "{{VERSION}}": args.version,
    }
    for token, value in substitutions.items():
        body = body.replace(token, value)

    missing = re.findall(r"\{\{[A-Z_]+\}\}", body)
    if missing:
        sys.exit("manual.py: unsubstituted placeholder(s): %s" % ", ".join(sorted(set(missing))))

    md = markdown.Markdown(extensions=EXTENSIONS, extension_configs=EXTENSION_CONFIGS)
    content = md.convert(body)

    accent = meta.get("accent", "#2E8B7A")
    css = pathlib.Path(args.css).read_text(encoding="utf-8").replace("ACCENT", accent)

    title = "%s %s — Manual" % (args.plugin, args.version)
    built = datetime.date.today().isoformat()

    document = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>{title}</title>
<style>
{css}
</style>
</head>
<body>
<div class="cover">
  {logo}
  <p class="plugin">{plugin}</p>
  <p class="tagline">{tagline}</p>
  <div class="rule"></div>
  <p class="meta">
    <strong>Version {version}</strong><br>
    {subtitle}<br>
    Part of the Audio Plugins<br>
    {built}
  </p>
</div>
{content}
</body>
</html>
""".format(
        title=html.escape(title),
        css=css,
        logo=logo_markup(args.logo, args.plugin),
        plugin=html.escape(args.plugin),
        tagline=html.escape(meta.get("tagline", "")),
        subtitle=html.escape(meta.get("subtitle", "CLAP instrument for Linux and Windows")),
        version=html.escape(args.version),
        built=built,
        content=content,
    )

    pathlib.Path(args.out).write_text(document, encoding="utf-8")


if __name__ == "__main__":
    main()
