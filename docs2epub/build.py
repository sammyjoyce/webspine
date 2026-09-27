from __future__ import annotations

import hashlib
import html
import mimetypes
import re
import shutil
import uuid
import zipfile
from dataclasses import dataclass
from datetime import UTC, datetime
from importlib.resources import files
from pathlib import Path
from typing import TYPE_CHECKING
from urllib.parse import urldefrag, urljoin

import cairosvg
from bs4 import BeautifulSoup, Comment
from lxml import (
    etree,
    html as lxml_html,
)
from PIL import Image, ImageDraw, ImageFont

from .scrape import canonical_url
from .workspace import Workspace, route_name

if TYPE_CHECKING:
    from .model import PageRecord

ALLOWED_TAGS = {
    "a",
    "abbr",
    "aside",
    "b",
    "blockquote",
    "br",
    "caption",
    "cite",
    "code",
    "dd",
    "del",
    "details",
    "dfn",
    "div",
    "dl",
    "dt",
    "em",
    "figcaption",
    "figure",
    "h1",
    "h2",
    "h3",
    "h4",
    "h5",
    "h6",
    "hr",
    "i",
    "img",
    "kbd",
    "li",
    "mark",
    "ol",
    "p",
    "pre",
    "q",
    "s",
    "samp",
    "section",
    "small",
    "span",
    "strong",
    "sub",
    "summary",
    "sup",
    "table",
    "tbody",
    "td",
    "tfoot",
    "th",
    "thead",
    "tr",
    "u",
    "ul",
    "var",
}
ALLOWED_ATTRIBUTES = {
    "id",
    "href",
    "src",
    "alt",
    "title",
    "colspan",
    "rowspan",
    "scope",
    "lang",
}


@dataclass
class BuiltBook:
    epub: Path
    chapters: int
    assets: int


def xml_id(value: str) -> str:
    value = re.sub(r"[^A-Za-z0-9_.-]+", "-", value.strip()).strip("-")
    if not value or not re.match(r"[A-Za-z_]", value):
        value = "id-" + value
    return value or "id-section"


def chapter_name(page: PageRecord) -> str:
    digest = hashlib.sha256(page.url.encode()).hexdigest()[:8]
    return f"{route_name(page.route)}-{digest}.xhtml"


def _normalize_headings(soup: BeautifulSoup) -> None:
    previous = 1
    for heading in soup.find_all(re.compile(r"^h[1-6]$")):
        level = int(heading.name[1])
        if level == 1:
            level = 2
        level = min(level, previous + 1)
        heading.name = f"h{level}"
        previous = level


def _convert_tabular_pre(soup: BeautifulSoup) -> None:
    for pre in list(soup.find_all("pre")):
        lines = [line.strip() for line in pre.get_text().splitlines() if line.strip()]
        if len(lines) < 3:
            continue
        rows = [re.split(r"\s{2,}", line) for line in lines]
        column_counts = [len(row) for row in rows]
        dominant = max(set(column_counts), key=column_counts.count)
        if dominant < 3 or column_counts.count(dominant) / len(rows) < 0.75:
            continue
        table = soup.new_tag("table")
        body = soup.new_tag("tbody")
        table.append(body)
        for row_index, values in enumerate(rows):
            row = soup.new_tag("tr")
            body.append(row)
            if len(values) != dominant:
                cell = soup.new_tag("td", colspan=str(dominant))
                cell.string = "  ".join(values)
                row.append(cell)
                continue
            for value in values:
                cell = soup.new_tag("th" if row_index == 0 else "td")
                if row_index == 0:
                    cell["scope"] = "col"
                cell.string = value
                row.append(cell)
        pre.replace_with(table)


def _clean_fragment(
    page: PageRecord,
    url_to_file: dict[str, str],
    asset_names: dict[str, str],
) -> str:
    soup = BeautifulSoup(page.html, "html.parser")
    first_heading = soup.find(re.compile(r"^h[1-6]$"))
    if first_heading and first_heading.get_text(" ", strip=True) == page.title:
        first_heading.decompose()
    _convert_tabular_pre(soup)
    for comment in soup.find_all(string=lambda value: isinstance(value, Comment)):
        comment.extract()
    for tag in list(soup.find_all(True)):
        if tag.name not in ALLOWED_TAGS:
            tag.unwrap()
            continue
        tag.attrs = {
            key: value
            for key, value in tag.attrs.items()
            if key in ALLOWED_ATTRIBUTES and not key.lower().startswith("on")
        }
    for details in soup.find_all("details"):
        details.name = "section"
        summary = details.find("summary", recursive=False)
        if summary:
            summary.name = "h3"
    _normalize_headings(soup)

    used_ids: set[str] = set()
    for tag in soup.find_all(True):
        if tag.get("id"):
            base = xml_id(str(tag["id"]))
            candidate = base
            suffix = 2
            while candidate in used_ids:
                candidate = f"{base}-{suffix}"
                suffix += 1
            tag["id"] = candidate
            used_ids.add(candidate)

    for link in soup.find_all("a", href=True):
        href = str(link["href"])
        if href.startswith(("mailto:", "tel:", "data:")):
            continue
        absolute = urljoin(page.url, href)
        target, fragment = urldefrag(absolute)
        local = url_to_file.get(canonical_url(target))
        if local:
            link["href"] = local + (f"#{xml_id(fragment)}" if fragment else "")
        elif href.startswith("#"):
            link["href"] = f"#{xml_id(href[1:])}"
        else:
            link["href"] = absolute

    for image in soup.find_all("img"):
        source = str(image.get("src", ""))
        name = Path(source).name
        if name in asset_names:
            image["src"] = f"../images/{asset_names[name]}"
        image["alt"] = str(image.get("alt") or "Illustration")
        if not image.parent or image.parent.name != "figure":
            figure = soup.new_tag("figure")
            image.wrap(figure)

    for table in soup.find_all("table"):
        if table.find("th"):
            continue
        first_row = table.find("tr")
        if first_row:
            for cell in first_row.find_all("td", recursive=False):
                cell.name = "th"
                cell["scope"] = "col"

    wrapper = lxml_html.fromstring(f"<div>{soup!s}</div>")
    return "".join(
        etree.tostring(child, encoding="unicode", method="xml") for child in wrapper
    )


def _chapter_xhtml(page: PageRecord, content: str, language: str) -> str:
    return f'''<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="{html.escape(language)}" lang="{html.escape(language)}">
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width, initial-scale=1"/>
  <title>{html.escape(page.title)}</title>
  <link rel="stylesheet" type="text/css" href="../styles/book.css"/>
</head>
<body>
  <main>
    <h1>{html.escape(page.title)}</h1>
    <p class="source">Source: <a href="{html.escape(page.url)}">{html.escape(page.url)}</a></p>
    {content}
  </main>
</body>
</html>
'''


def _cover_image(title: str, destination: Path) -> None:
    image = Image.new("RGB", (1200, 1600), "#f4f0e7")
    draw = ImageDraw.Draw(image)
    font = ImageFont.load_default(size=76)
    small = ImageFont.load_default(size=30)
    words = title.split()
    lines: list[str] = []
    current = ""
    for word in words:
        candidate = f"{current} {word}".strip()
        if draw.textlength(candidate, font=font) > 980 and current:
            lines.append(current)
            current = word
        else:
            current = candidate
    if current:
        lines.append(current)
    y = 520
    for line in lines:
        box = draw.textbbox((0, 0), line, font=font)
        draw.text(((1200 - (box[2] - box[0])) / 2, y), line, fill="#24211d", font=font)
        y += 100
    label = "DOCUMENTATION"
    width = draw.textlength(label, font=small)
    draw.text(((1200 - width) / 2, 390), label, fill="#685f52", font=small)
    image.save(destination, "JPEG", quality=90, optimize=True)


def _normalize_assets(workspace: Workspace, images_dir: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for source in sorted(workspace.assets.iterdir()):
        if not source.is_file():
            continue
        output_name = source.name
        try:
            if source.suffix.lower() == ".svg":
                output_name = source.stem + ".png"
                cairosvg.svg2png(
                    url=str(source),
                    write_to=str(images_dir / output_name),
                    output_width=1600,
                )
            else:
                with Image.open(source) as image:
                    image.thumbnail((2000, 2400), Image.Resampling.LANCZOS)
                    if image.format in {"WEBP", "AVIF", "BMP", "TIFF"}:
                        if image.mode not in ("RGB", "RGBA"):
                            image = image.convert("RGBA")
                        output_name = source.stem + ".png"
                        image.save(images_dir / output_name, "PNG", optimize=True)
                    else:
                        image.save(images_dir / output_name)
        except Exception:
            shutil.copy2(source, images_dir / output_name)
        result[source.name] = output_name
    return result


def _manifest_media_type(path: Path) -> str:
    if path.suffix == ".xhtml":
        return "application/xhtml+xml"
    if path.suffix == ".css":
        return "text/css"
    return mimetypes.guess_type(path.name)[0] or "application/octet-stream"


def _write_package(workspace: Workspace, output: Path) -> BuiltBook:
    site = workspace.read_site()
    pages_by_url = {page.url: page for page in workspace.read_pages()}
    ordered_pages = [
        pages_by_url[node.url] for node in site.nav if node.url in pages_by_url
    ]
    seen = {page.url for page in ordered_pages}
    ordered_pages.extend(page for page in pages_by_url.values() if page.url not in seen)
    if not ordered_pages:
        raise RuntimeError("The workspace has no captured pages.")

    if workspace.build.exists():
        shutil.rmtree(workspace.build)
    epub_root = workspace.build / "EPUB"
    text_dir = epub_root / "text"
    styles_dir = epub_root / "styles"
    images_dir = epub_root / "images"
    meta_dir = workspace.build / "META-INF"
    for path in (text_dir, styles_dir, images_dir, meta_dir):
        path.mkdir(parents=True, exist_ok=True)

    (workspace.build / "mimetype").write_text("application/epub+zip")
    (meta_dir / "container.xml").write_text("""<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles><rootfile full-path="EPUB/package.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>
""")
    shutil.copyfile(files("docs2epub").joinpath("book.css"), styles_dir / "book.css")
    cover_path = images_dir / "cover.jpg"
    _cover_image(site.title, cover_path)
    asset_names = _normalize_assets(workspace, images_dir)

    url_to_file = {page.url: chapter_name(page) for page in ordered_pages}
    chapter_items: list[tuple[str, str, str]] = []
    for index, page in enumerate(ordered_pages, start=1):
        file_name = url_to_file[page.url]
        content = _clean_fragment(page, url_to_file, asset_names)
        (text_dir / file_name).write_text(
            _chapter_xhtml(page, content, page.language or site.language)
        )
        chapter_items.append((f"chapter-{index}", file_name, page.title))

    (epub_root / "cover.xhtml").write_text(f'''<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="{html.escape(site.language)}" lang="{html.escape(site.language)}">
<head><title>{html.escape(site.title)}</title><link rel="stylesheet" type="text/css" href="styles/book.css"/></head>
<body><section class="cover" epub:type="cover"><img src="images/cover.jpg" alt="Cover for {html.escape(site.title)}"/></section></body>
</html>
''')
    nav_items = "\n".join(
        f'<li><a href="text/{html.escape(file_name)}">{html.escape(title)}</a></li>'
        for _, file_name, title in chapter_items
    )
    (epub_root / "nav.xhtml").write_text(f'''<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="{html.escape(site.language)}" lang="{html.escape(site.language)}">
<head><title>Contents</title><link rel="stylesheet" type="text/css" href="styles/book.css"/></head>
<body><nav epub:type="toc" id="toc"><h1>Contents</h1><ol>{nav_items}</ol></nav>
<nav epub:type="landmarks" hidden="hidden"><ol>
<li><a epub:type="cover" href="cover.xhtml">Cover</a></li>
<li><a epub:type="toc" href="nav.xhtml">Contents</a></li>
<li><a epub:type="bodymatter" href="text/{html.escape(chapter_items[0][1])}">Start</a></li>
</ol></nav></body>
</html>
''')
    ncx_points = "\n".join(
        f'<navPoint id="nav-{index}" playOrder="{index}"><navLabel><text>{html.escape(title)}</text></navLabel><content src="text/{html.escape(file_name)}"/></navPoint>'
        for index, (_, file_name, title) in enumerate(chapter_items, start=1)
    )
    identifier = f"urn:uuid:{uuid.uuid5(uuid.NAMESPACE_URL, site.base_url)}"
    (epub_root / "toc.ncx").write_text(f'''<?xml version="1.0" encoding="utf-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1" xml:lang="{html.escape(site.language)}">
<head><meta name="dtb:uid" content="{identifier}"/></head><docTitle><text>{html.escape(site.title)}</text></docTitle><navMap>{ncx_points}</navMap>
</ncx>
''')

    manifest = [
        '<item id="cover-page" href="cover.xhtml" media-type="application/xhtml+xml"/>',
        '<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>',
        '<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>',
        '<item id="css" href="styles/book.css" media-type="text/css"/>',
        '<item id="cover-image" href="images/cover.jpg" media-type="image/jpeg" properties="cover-image"/>',
    ]
    manifest.extend(
        f'<item id="{item_id}" href="text/{html.escape(file_name)}" media-type="application/xhtml+xml"/>'
        for item_id, file_name, _ in chapter_items
    )
    for index, asset in enumerate(sorted(images_dir.iterdir()), start=1):
        if asset.name == "cover.jpg":
            continue
        manifest.append(
            f'<item id="asset-{index}" href="images/{html.escape(asset.name)}" media-type="{_manifest_media_type(asset)}"/>'
        )
    spine = "\n".join(
        ['<itemref idref="cover-page"/>', '<itemref idref="nav" linear="no"/>']
        + [f'<itemref idref="{item_id}"/>' for item_id, _, _ in chapter_items]
    )
    modified = (
        datetime.now(UTC).replace(microsecond=0).isoformat().replace("+00:00", "Z")
    )
    (epub_root / "package.opf").write_text(f'''<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="book-id" xml:lang="{html.escape(site.language)}" prefix="schema: http://schema.org/">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
  <dc:identifier id="book-id">{identifier}</dc:identifier>
  <dc:title>{html.escape(site.title)}</dc:title>
  <dc:language>{html.escape(site.language)}</dc:language>
  <dc:source>{html.escape(site.base_url)}</dc:source>
  <dc:publisher>docs2epub</dc:publisher>
  <meta property="dcterms:modified">{modified}</meta>
  <meta property="rendition:layout">reflowable</meta>
  <meta property="rendition:spread">auto</meta>
  <meta property="schema:accessMode">textual</meta>
  <meta property="schema:accessMode">visual</meta>
  <meta property="schema:accessibilityFeature">alternativeText</meta>
  <meta property="schema:accessibilityFeature">readingOrder</meta>
  <meta property="schema:accessibilityFeature">tableOfContents</meta>
  <meta property="schema:accessibilityHazard">none</meta>
  <meta property="schema:accessibilitySummary">The publication has a table of contents, source links, reflowable text, and alternative text for images.</meta>
</metadata>
<manifest>{"".join(manifest)}</manifest>
<spine toc="ncx">{spine}</spine>
</package>
''')

    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    with zipfile.ZipFile(temporary, "w") as archive:
        archive.write(
            workspace.build / "mimetype", "mimetype", compress_type=zipfile.ZIP_STORED
        )
        for path in sorted(workspace.build.rglob("*")):
            if path.is_file() and path.name != "mimetype":
                archive.write(
                    path,
                    path.relative_to(workspace.build),
                    compress_type=zipfile.ZIP_DEFLATED,
                )
    temporary.replace(output)
    return BuiltBook(epub=output, chapters=len(chapter_items), assets=len(asset_names))


def build(workspace_path: Path, output: Path | None = None) -> BuiltBook:
    workspace = Workspace(workspace_path)
    site = workspace.read_site()
    destination = output or workspace.dist / f"{route_name(site.title)}.epub"
    return _write_package(workspace, destination)
