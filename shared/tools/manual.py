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
            --params params.md --params-brief params-brief.md --presets presets.md \
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


# ---------------------------------------------------------------- page breaks
#
# `page-break-after: avoid` on a heading is in the stylesheet and wkhtmltopdf's
# WebKit ignores it, which is how a section heading ends up alone at the foot of
# a page with its content overleaf. What that build *does* honour is
# `page-break-inside: avoid` on a block -- so the heading and the block under it
# are wrapped in one, and the pair moves to the next page together or not at
# all.
#
# The exception is a long table. Binding a heading to a table half a page deep
# would push both to a fresh page and leave the hole it was meant to close, so
# past a threshold the heading is left to take its chances.
KEEP_WITH_HEADING = re.compile(r"^<h([234])[ >]")
MAX_KEPT_ROWS = 10


def top_level_blocks(html_text):
    """Splits the converted body into its top-level elements.

    python-markdown emits a flat sequence of them, so this only has to track
    the depth of the tags it opens and closes rather than parse anything.
    """
    blocks, depth, start = [], 0, 0
    for m in re.finditer(r"<(/?)([a-zA-Z][a-zA-Z0-9]*)([^>]*)>", html_text):
        closing, name, rest = m.group(1), m.group(2).lower(), m.group(3)
        if name in ("br", "hr", "img", "meta", "link") or rest.endswith("/"):
            continue
        if closing:
            depth -= 1
            if depth == 0:
                blocks.append(html_text[start : m.end()])
                start = m.end()
        else:
            if depth == 0:
                between = html_text[start : m.start()]
                if between.strip():
                    blocks.append(between)
                start = m.start()
            depth += 1
    tail = html_text[start:]
    if tail.strip():
        blocks.append(tail)
    return blocks


def keep_headings_with_content(html_text):
    blocks = top_level_blocks(html_text)
    out, i = [], 0
    while i < len(blocks):
        block = blocks[i]
        following = blocks[i + 1] if i + 1 < len(blocks) else ""
        long_table = following.lstrip().startswith("<table") and (
            following.count("<tr") > MAX_KEPT_ROWS
        )
        if KEEP_WITH_HEADING.match(block.lstrip()) and following.strip() and not long_table:
            out.append('<div class="keep">%s\n%s</div>' % (block, following))
            i += 2
            continue
        out.append(block)
        i += 1
    return "\n".join(out)


def data_uri(path):
    suffix = pathlib.Path(path).suffix.lower()
    mime = {".png": "image/png", ".svg": "image/svg+xml", ".jpg": "image/jpeg"}.get(
        suffix, "application/octet-stream"
    )
    return "data:%s;base64,%s" % (
        mime,
        base64.b64encode(pathlib.Path(path).read_bytes()).decode("ascii"),
    )


# Markdown image references, resolved against the manual source's own folder
# and inlined. The output has to stand on its own -- it is published as a web
# manual and wkhtmltopdf renders it with no access to the file system -- so a
# relative <img src> would be a broken image in both.
IMG_SRC = re.compile(r'(<img\b[^>]*?\bsrc=")([^"]+)(")')


def inline_images(html_text, base_dir):
    def one(match):
        src = match.group(2)
        if src.startswith(("data:", "http:", "https:")):
            return match.group(0)
        path = (base_dir / src).resolve()
        if not path.is_file():
            sys.exit("manual.py: no such image: %s" % src)
        return match.group(1) + data_uri(path) + match.group(3)

    return IMG_SRC.sub(one, html_text)


# A figure is an image on a paragraph of its own, optionally with the alt text
# under it as a caption. Markdown has no syntax for one, so the shape it does
# produce -- a <p> holding nothing but an <img> -- is promoted here.
LONE_IMAGE = re.compile(r"<p>\s*(<img\b[^>]*>)\s*</p>")


def figures(html_text):
    def one(match):
        img = match.group(1)
        alt = re.search(r'\balt="([^"]*)"', img)
        caption = ""
        if alt and alt.group(1):
            caption = "<figcaption>%s</figcaption>" % alt.group(1)
        return '<figure>%s%s</figure>' % (img, caption)

    return LONE_IMAGE.sub(one, html_text)


def logo_markup(path, plugin):
    """The cover image, when there is one.

    There used to be a fallback here: with no logo file the cover printed the
    plugin's name as a small letter-spaced wordmark. That put the name on the
    cover *twice*, because the line below it is the name as well -- once in
    22pt capitals and once in 40pt. A collection logo is a mark for the
    collection, and standing the plugin's own name in for it says nothing the
    next line does not say louder. So there is no fallback: without a logo the
    cover simply starts at the name.
    """
    if path and pathlib.Path(path).is_file():
        return '<img src="%s" alt="%s">' % (data_uri(path), html.escape(plugin))
    return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plugin", required=True)
    # What the plugin is *called*, where that is not what it is filed under.
    # SaeureKiste is the CMake project, the binary and the preset directory,
    # and none of those may carry an umlaut; the instrument is SaeureKiste's
    # display name and is what the cover and the prose say. Defaults to
    # --plugin, so a plugin whose two names agree passes nothing.
    ap.add_argument("--display-name", default="")
    ap.add_argument("--version", required=True)
    ap.add_argument("--source", required=True)
    ap.add_argument("--params", required=True)
    # The same table without the explanations, for a manual that wants a
    # drift-proof reference beside its own prose rather than instead of it.
    ap.add_argument("--params-brief", dest="params_brief", default="")
    ap.add_argument("--presets", required=True)
    ap.add_argument("--css", required=True)
    # Optional: a collection that has no logo yet still builds a manual, it
    # just gets a wordmark on the cover instead of an image.
    ap.add_argument("--logo", default="")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    display = args.display_name or args.plugin

    source = pathlib.Path(args.source).read_text(encoding="utf-8")
    meta, body = split_header(source)

    substitutions = {
        "{{PARAMETER_REFERENCE}}": pathlib.Path(args.params).read_text(encoding="utf-8"),
        # Left unsubstituted when no brief table was supplied, so the check
        # below catches it. Substituting an empty string instead would drop
        # the appendix out of the manual without saying anything -- which is
        # exactly how the generated chapters went missing in the first place.
        **(
            {"{{PARAMETER_SUMMARY}}": pathlib.Path(args.params_brief).read_text(encoding="utf-8")}
            if args.params_brief
            else {}
        ),
        "{{PRESET_LIBRARY}}": pathlib.Path(args.presets).read_text(encoding="utf-8"),
        "{{PLUGIN}}": display,
        "{{VERSION}}": args.version,
    }
    for token, value in substitutions.items():
        body = body.replace(token, value)

    missing = re.findall(r"\{\{[A-Z_]+\}\}", body)
    if missing:
        sys.exit("manual.py: unsubstituted placeholder(s): %s" % ", ".join(sorted(set(missing))))

    md = markdown.Markdown(extensions=EXTENSIONS, extension_configs=EXTENSION_CONFIGS)
    base_dir = pathlib.Path(args.source).resolve().parent
    content = figures(inline_images(md.convert(body), base_dir))
    content = keep_headings_with_content(content)

    accent = meta.get("accent", "#2E8B7A")
    css = pathlib.Path(args.css).read_text(encoding="utf-8").replace("ACCENT", accent)

    title = "%s %s — Manual" % (display, args.version)
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
        logo=logo_markup(args.logo, display),
        plugin=html.escape(display),
        tagline=html.escape(meta.get("tagline", "")),
        subtitle=html.escape(meta.get("subtitle", "CLAP instrument for Linux and Windows")),
        version=html.escape(args.version),
        built=built,
        content=content,
    )

    pathlib.Path(args.out).write_text(document, encoding="utf-8")


if __name__ == "__main__":
    main()
