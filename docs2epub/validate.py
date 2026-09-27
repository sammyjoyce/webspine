from __future__ import annotations

import json
import re
import shutil
import subprocess
import zipfile
from dataclasses import asdict
from typing import TYPE_CHECKING
from urllib.parse import urldefrag

from bs4 import BeautifulSoup
from lxml import etree
from playwright.sync_api import sync_playwright

from .model import Finding, StageResult
from .workspace import Workspace

if TYPE_CHECKING:
    from pathlib import Path


def _extract(epub: Path, destination: Path) -> Path:
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)
    with zipfile.ZipFile(epub) as archive:
        archive.extractall(destination)
    return destination


def _package_findings(epub: Path, extracted: Path) -> list[Finding]:
    findings: list[Finding] = []
    with zipfile.ZipFile(epub) as archive:
        names = archive.namelist()
        if not names or names[0] != "mimetype":
            findings.append(
                Finding(
                    "EPUB_MIMETYPE_ORDER",
                    "error",
                    "mimetype is not the first ZIP entry.",
                    "validate",
                )
            )
        elif archive.getinfo("mimetype").compress_type != zipfile.ZIP_STORED:
            findings.append(
                Finding(
                    "EPUB_MIMETYPE_COMPRESSED",
                    "error",
                    "mimetype must be stored without compression.",
                    "validate",
                )
            )

    xhtml_files = sorted(extracted.rglob("*.xhtml"))
    known = {path.resolve() for path in xhtml_files}
    for path in xhtml_files:
        try:
            etree.parse(str(path))
        except etree.XMLSyntaxError as error:
            findings.append(
                Finding(
                    "XHTML_INVALID", "error", str(error), "validate", file=str(path)
                )
            )
            continue
        soup = BeautifulSoup(path.read_text(), "xml")
        root = soup.find("html")
        if not root or not (root.get("lang") or root.get("xml:lang")):
            findings.append(
                Finding(
                    "A11Y_LANGUAGE",
                    "error",
                    "The XHTML document has no language.",
                    "validate",
                    file=str(path),
                )
            )
        for image in soup.find_all("img"):
            if not image.get("alt"):
                findings.append(
                    Finding(
                        "A11Y_IMAGE_ALT",
                        "error",
                        "An image has no alt text.",
                        "validate",
                        file=str(path),
                    )
                )
        for table in soup.find_all("table"):
            if not table.find("th"):
                findings.append(
                    Finding(
                        "A11Y_TABLE_HEADER",
                        "error",
                        "A table has no header cells.",
                        "validate",
                        file=str(path),
                    )
                )
        previous = 0
        for heading in soup.find_all([f"h{level}" for level in range(1, 7)]):
            level = int(heading.name[1])
            if previous and level > previous + 1:
                findings.append(
                    Finding(
                        "A11Y_HEADING_ORDER",
                        "error",
                        f"Heading level jumps from h{previous} to h{level}.",
                        "validate",
                        file=str(path),
                    )
                )
            previous = level
        for link in soup.find_all("a", href=True):
            href = str(link["href"])
            if href.startswith(
                ("http://", "https://", "mailto:", "tel:")
            ) or href.startswith("#"):
                continue
            target, _ = urldefrag(href)
            resolved = (path.parent / target).resolve()
            if resolved.suffix == ".xhtml" and resolved not in known:
                findings.append(
                    Finding(
                        "LINK_BROKEN_INTERNAL",
                        "error",
                        f"Internal link target does not exist: {href}",
                        "validate",
                        file=str(path),
                    )
                )
    return findings


def _epubcheck(epub: Path, checks: Path) -> list[Finding]:
    executable = shutil.which("epubcheck")
    if not executable:
        return [
            Finding(
                "ENV_EPUBCHECK_MISSING",
                "error",
                "epubcheck is not on PATH.",
                "validate",
                hint="Run through nix run or nix develop.",
            )
        ]
    report = checks / "epubcheck.json"
    process = subprocess.run(
        [executable, str(epub), "-j", str(report)],
        text=True,
        capture_output=True,
        check=False,
    )
    (checks / "epubcheck.txt").write_text(process.stdout + process.stderr)
    if process.returncode == 0:
        return []
    message = process.stdout.strip() or process.stderr.strip() or "EPUBCheck failed."
    return [
        Finding(
            "EPUBCHECK_FAILED", "error", message[-4000:], "validate", file=str(epub)
        )
    ]


def _reflow(extracted: Path, checks: Path) -> list[Finding]:
    findings: list[Finding] = []
    results: list[dict[str, object]] = []
    xhtml_files = sorted((extracted / "EPUB" / "text").glob("*.xhtml"))
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch()
        page = browser.new_page()
        for width, scale in ((320, 1.0), (390, 1.5), (768, 2.0)):
            page.set_viewport_size({"width": width, "height": 900})
            for path in xhtml_files:
                page.goto(path.resolve().as_uri(), wait_until="load")
                page.evaluate(
                    "scale => document.documentElement.style.fontSize = `${scale * 100}%`",
                    scale,
                )
                overflow = page.evaluate("""() => ({
                    documentWidth: document.documentElement.scrollWidth,
                    viewportWidth: document.documentElement.clientWidth,
                    offenders: [...document.querySelectorAll('pre,table,img,svg')]
                      .filter((node) => node.getBoundingClientRect().right > document.documentElement.clientWidth + 1)
                      .slice(0, 10)
                      .map((node) => node.tagName.toLowerCase())
                })""")
                result = {"file": path.name, "width": width, "scale": scale, **overflow}
                results.append(result)
                if (
                    overflow["documentWidth"] > overflow["viewportWidth"] + 1
                    or overflow["offenders"]
                ):
                    findings.append(
                        Finding(
                            "REFLOW_OVERFLOW_X",
                            "error",
                            f"Horizontal overflow at {width}px and {scale:.1f}x text: {overflow}",
                            "validate",
                            file=str(path),
                        )
                    )
        browser.close()
    (checks / "reflow.json").write_text(json.dumps(results, indent=2) + "\n")
    return findings


def _coverage(workspace: Workspace) -> list[Finding]:
    site = workspace.read_site()
    captured = {page.url for page in workspace.read_pages()}
    expected = set(site.sitemap_urls)
    expected.update(node.url for node in site.nav if node.url)
    missing = sorted(expected - captured)
    findings = [
        Finding(
            "COVERAGE_MISSING_PAGE",
            "error",
            "A discovered page was not captured.",
            "validate",
            url=url,
        )
        for url in missing
    ]
    source = workspace.root / "source" / "llms-full.txt"
    if not source.exists():
        return findings
    source_lines: list[str] = []
    in_code = False
    for raw_line in source.read_text(errors="replace").splitlines():
        line = raw_line.strip()
        if line.startswith("```"):
            in_code = not in_code
            continue
        if in_code or len(line) < 40 or line.startswith(("#", "|", "![", "<")):
            continue
        line = re.sub(r"^[*+-]\s+", "", line)
        line = re.sub(r"[*_`\\]", "", line)
        normalized = " ".join(line.lower().split())
        if normalized:
            source_lines.append(normalized)
    chapter_text = " ".join(
        " ".join(BeautifulSoup(path.read_text(), "xml").get_text(" ").lower().split())
        for path in sorted((workspace.build / "EPUB" / "text").glob("*.xhtml"))
    )
    missing_lines = [line for line in source_lines if line not in chapter_text]
    coverage = 1.0 if not source_lines else 1 - len(missing_lines) / len(source_lines)
    workspace.write_json(
        workspace.checks / "coverage.json",
        {"checked": len(source_lines), "missing": missing_lines, "coverage": coverage},
    )
    if coverage < 0.9:
        findings.append(
            Finding(
                "COVERAGE_TEXT_BELOW_THRESHOLD",
                "error",
                f"Text coverage is {coverage:.1%}; expected at least 90%.",
                "validate",
                file=str(source),
            )
        )
    return findings


def validate(
    epub: Path, workspace_path: Path | None = None, reflow: bool = True
) -> StageResult:
    epub = epub.resolve()
    checks = (
        Workspace(workspace_path).checks
        if workspace_path
        else epub.parent / f"{epub.stem}-checks"
    )
    checks.mkdir(parents=True, exist_ok=True)
    extracted = _extract(epub, checks / "extracted")
    findings = _package_findings(epub, extracted)
    findings.extend(_epubcheck(epub, checks))
    if workspace_path:
        findings.extend(_coverage(Workspace(workspace_path)))
    if reflow:
        findings.extend(_reflow(extracted, checks))
    result = StageResult(
        stage="validate",
        status="failed"
        if any(item.severity == "error" for item in findings)
        else "passed",
        counts={
            "errors": sum(item.severity == "error" for item in findings),
            "warnings": sum(item.severity == "warning" for item in findings),
        },
        findings=findings,
    )
    (checks / "validation.json").write_text(json.dumps(asdict(result), indent=2) + "\n")
    return result
