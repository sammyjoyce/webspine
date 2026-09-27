from __future__ import annotations

import hashlib
import mimetypes
import re
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import urldefrag, urljoin, urlparse, urlunparse
from urllib.request import Request, urlopen
from xml.etree import ElementTree

import cairosvg
from bs4 import BeautifulSoup
from playwright.sync_api import BrowserContext, Page, sync_playwright

from .model import IR_VERSION, NavNode, PageRecord, SiteRecord
from .workspace import Workspace

CONTENT_SELECTORS = (
    "main article",
    "article",
    "main [class*='content']",
    "main",
    "[role='main']",
)

NAV_SELECTORS = (
    "#sidebar-content",
    "nav[aria-label*='doc' i]",
    "aside nav",
    "aside",
)

EXTRACT_JS = """
({contentSelectors, navSelectors}) => {
  document.querySelectorAll('details').forEach((node) => node.open = true);
  document.querySelectorAll('[aria-expanded="false"]').forEach((node) => {
    try { node.click(); } catch (_) {}
  });
  document.querySelectorAll('[role="tab"]').forEach((tab) => {
    const panelId = tab.getAttribute('aria-controls');
    const panel = panelId ? document.getElementById(panelId) : null;
    if (panel) {
      panel.hidden = false;
      panel.removeAttribute('aria-hidden');
      const label = document.createElement('h3');
      label.textContent = tab.textContent.trim();
      panel.prepend(label);
    }
  });
  const content = contentSelectors.map((s) => document.querySelector(s)).find(Boolean);
  if (!content) return {error: 'content-not-found'};
  content.querySelectorAll('img').forEach((image) => {
    if (image.currentSrc) image.src = image.currentSrc;
    image.removeAttribute('srcset');
  });
  content.querySelectorAll('*').forEach((node) => {
    if (getComputedStyle(node).display === 'none') node.remove();
  });
  const clone = content.cloneNode(true);
  clone.querySelectorAll('script,style,noscript,button,input,textarea,select,nav,[role="navigation"],[aria-hidden="true"]').forEach((node) => node.remove());
  clone.querySelectorAll('*').forEach((node) => {
    node.removeAttribute('style');
    node.removeAttribute('class');
    node.removeAttribute('onclick');
    if (node.tagName === 'IFRAME') {
      const link = document.createElement('a');
      link.href = node.src;
      link.textContent = node.title || 'Embedded content';
      node.replaceWith(link);
    }
  });
  const nav = navSelectors.map((s) => document.querySelector(s)).find(Boolean);
  const links = nav ? [...nav.querySelectorAll('a[href]')].map((a) => ({
    title: a.textContent.trim(), href: a.href
  })) : [];
  return {
    title: document.querySelector('h1')?.textContent.trim() || document.title,
    language: document.documentElement.lang || 'en',
    html: clone.innerHTML,
    nav: links,
  };
}
"""


@dataclass
class ScrapeOptions:
    url: str
    workspace: Path
    title: str | None = None
    language: str | None = None
    content_selector: str | None = None
    nav_selector: str | None = None
    max_pages: int = 500
    force: bool = False


def canonical_url(url: str) -> str:
    clean, _ = urldefrag(url)
    parsed = urlparse(clean)
    path = re.sub(r"/{2,}", "/", parsed.path or "/")
    if path != "/":
        path = path.rstrip("/")
    return urlunparse((parsed.scheme, parsed.netloc, path, "", "", ""))


def in_scope(url: str, base_url: str) -> bool:
    target = urlparse(url)
    base = urlparse(base_url)
    base_path = base.path.rstrip("/")
    return (
        target.scheme in ("http", "https")
        and target.netloc == base.netloc
        and (
            not base_path
            or target.path == base_path
            or target.path.startswith(base_path + "/")
        )
    )


def sitemap_urls(base_url: str) -> list[str]:
    base = urlparse(base_url)
    candidates = [
        base_url.rstrip("/") + "/sitemap.xml",
        f"{base.scheme}://{base.netloc}/sitemap.xml",
    ]
    for candidate in dict.fromkeys(candidates):
        try:
            request = Request(candidate, headers={"User-Agent": "docs2epub/0.1"})
            with urlopen(request, timeout=20) as response:
                root = ElementTree.fromstring(response.read())
            urls = [
                canonical_url(node.text.strip())
                for node in root.findall(".//{*}loc")
                if node.text and in_scope(node.text.strip(), base_url)
            ]
            if urls:
                return list(dict.fromkeys(urls))
        except Exception:
            continue
    return []


def _cache_text_source(base_url: str, workspace: Workspace) -> None:
    base = urlparse(base_url)
    candidates = [
        base_url.rstrip("/") + "/llms-full.txt",
        f"{base.scheme}://{base.netloc}/llms-full.txt",
    ]
    for candidate in dict.fromkeys(candidates):
        try:
            request = Request(candidate, headers={"User-Agent": "docs2epub/0.1"})
            with urlopen(request, timeout=20) as response:
                body = response.read()
            if body.strip():
                source = workspace.root / "source"
                source.mkdir(exist_ok=True)
                (source / "llms-full.txt").write_bytes(body)
                return
        except Exception:
            continue


def _asset_name(url: str, media_type: str | None) -> str:
    extension = mimetypes.guess_extension((media_type or "").split(";", 1)[0])
    if not extension:
        extension = Path(urlparse(url).path).suffix or ".bin"
    return hashlib.sha256(url.encode()).hexdigest()[:20] + extension


def _download_assets(
    context: BrowserContext,
    page_url: str,
    html: str,
    workspace: Workspace,
) -> tuple[str, list[str], list[str]]:
    soup = BeautifulSoup(html, "html.parser")
    assets: list[str] = []
    warnings: list[str] = []
    for diagram in list(soup.find_all("svg")):
        try:
            data = str(diagram).encode()
            name = hashlib.sha256(data).hexdigest()[:20] + ".png"
            path = workspace.assets / name
            if not path.exists():
                cairosvg.svg2png(bytestring=data, write_to=str(path), output_width=1600)
            replacement = soup.new_tag("img")
            replacement["src"] = f"assets/{name}"
            replacement["alt"] = (
                diagram.get("aria-label") or diagram.get("title") or "Diagram"
            )
            diagram.replace_with(replacement)
            assets.append(name)
        except Exception as error:
            warnings.append(f"Could not rasterize an inline SVG: {error}")
    for image in soup.find_all("img"):
        source = image.get("src")
        if not source or source.startswith("data:") or Path(source).name in assets:
            continue
        url = urljoin(page_url, source)
        try:
            response = context.request.get(url, timeout=30_000)
            if not response.ok:
                raise RuntimeError(f"HTTP {response.status}")
            media_type = response.headers.get("content-type")
            name = _asset_name(url, media_type)
            path = workspace.assets / name
            if not path.exists():
                path.write_bytes(response.body())
            image["src"] = f"assets/{name}"
            image.attrs.pop("srcset", None)
            assets.append(name)
        except Exception as error:
            warnings.append(f"Could not download image {url}: {error}")
    return str(soup), assets, warnings


def _render(
    page: Page, url: str, content: tuple[str, ...], nav: tuple[str, ...]
) -> dict[str, object]:
    page.goto(url, wait_until="networkidle", timeout=60_000)
    page.emulate_media(color_scheme="light", reduced_motion="reduce")
    page.evaluate("window.scrollTo(0, document.body.scrollHeight)")
    page.wait_for_timeout(100)
    return page.evaluate(EXTRACT_JS, {"contentSelectors": content, "navSelectors": nav})


def scrape(options: ScrapeOptions) -> SiteRecord:
    base_url = canonical_url(options.url)
    workspace = Workspace(options.workspace)
    workspace.create()
    _cache_text_source(base_url, workspace)
    content_selectors = (
        (options.content_selector,) if options.content_selector else ()
    ) + CONTENT_SELECTORS
    nav_selectors = (
        (options.nav_selector,) if options.nav_selector else ()
    ) + NAV_SELECTORS
    sitemap = sitemap_urls(base_url)
    cached = {page.url: page for page in workspace.read_pages()}

    with sync_playwright() as playwright:
        browser = playwright.chromium.launch()
        context = browser.new_context(
            viewport={"width": 1280, "height": 900}, color_scheme="light"
        )
        page = context.new_page()
        first = _render(page, base_url, content_selectors, nav_selectors)
        if first.get("error"):
            raise RuntimeError(
                "No documentation content root was found. Pass --content-selector for this site."
            )
        nav_links = [
            (str(link["title"]).strip(), canonical_url(str(link["href"])))
            for link in first.get("nav", [])
            if str(link.get("title", "")).strip()
            and in_scope(str(link.get("href", "")), base_url)
        ]
        unique_nav_links: list[tuple[str, str]] = []
        nav_urls: set[str] = set()
        for title, url in nav_links:
            if url not in nav_urls:
                unique_nav_links.append((title, url))
                nav_urls.add(url)
        nav_links = unique_nav_links
        discovered = list(
            dict.fromkeys([base_url, *(url for _, url in nav_links), *sitemap])
        )
        if len(discovered) > options.max_pages:
            browser.close()
            raise RuntimeError(
                f"Discovered {len(discovered)} pages, above --max-pages {options.max_pages}."
            )

        for url in discovered:
            if url in cached and not options.force:
                continue
            rendered = (
                first
                if url == base_url
                else _render(page, url, content_selectors, nav_selectors)
            )
            if rendered.get("error"):
                continue
            html, assets, warnings = _download_assets(
                context,
                url,
                str(rendered["html"]),
                workspace,
            )
            sources = []
            if url in sitemap:
                sources.append("sitemap")
            if url == base_url or any(link_url == url for _, link_url in nav_links):
                sources.append("navigation")
            record = PageRecord(
                url=url,
                route=urlparse(url).path or "/",
                title=str(rendered["title"]).strip(),
                language=options.language or str(rendered["language"]),
                html=html,
                content_hash=hashlib.sha256(html.encode()).hexdigest(),
                discovered_from=sources or ["entrypoint"],
                assets=assets,
                warnings=warnings,
            )
            workspace.write_page(record)
        browser.close()

    pages = workspace.read_pages()
    page_urls = {record.url for record in pages}
    nav = [
        NavNode(title=title, url=url) for title, url in nav_links if url in page_urls
    ]
    nav_urls = {node.url for node in nav}
    for url in discovered:
        if url in page_urls and url not in nav_urls:
            title = next(record.title for record in pages if record.url == url)
            nav.append(NavNode(title=title, url=url))
    site = SiteRecord(
        base_url=base_url,
        title=options.title or str(first["title"]),
        language=options.language or str(first["language"]),
        adapter="generic",
        ir_version=IR_VERSION,
        sitemap_urls=sitemap,
        nav=nav,
        pages=sorted(page_urls),
    )
    workspace.write_site(site)
    return site
