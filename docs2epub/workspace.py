from __future__ import annotations

import hashlib
import json
import re
from dataclasses import asdict
from pathlib import Path
from urllib.parse import urlparse

from .model import IR_VERSION, NavNode, PageRecord, SiteRecord


def default_workspace(url: str) -> Path:
    parsed = urlparse(url)
    path = parsed.path.strip("/").replace("/", "-") or "root"
    return Path(".docs2epub") / f"{parsed.netloc}-{path}"


def route_name(route: str) -> str:
    value = route.strip("/") or "index"
    value = re.sub(r"[^A-Za-z0-9._-]+", "-", value)
    value = value.strip("-") or "index"
    return "index" if value in (".", "..") else value


def page_key(url: str) -> str:
    return hashlib.sha256(url.encode()).hexdigest()[:16]


class Workspace:
    def __init__(self, root: Path):
        self.root = root.resolve()
        self.pages = self.root / "pages"
        self.assets = self.root / "assets"
        self.build = self.root / "build"
        self.dist = self.root / "dist"
        self.checks = self.root / "checks"

    def create(self) -> None:
        for path in (self.pages, self.assets, self.build, self.dist, self.checks):
            path.mkdir(parents=True, exist_ok=True)

    def write_json(self, path: Path, value: object) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_suffix(path.suffix + ".tmp")
        temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")
        temporary.replace(path)

    def write_site(self, site: SiteRecord) -> None:
        self.write_json(self.root / "site.json", asdict(site))

    def read_site(self) -> SiteRecord:
        value = json.loads((self.root / "site.json").read_text())
        if value["ir_version"] != IR_VERSION:
            raise ValueError(
                f"Workspace IR version {value['ir_version']} is not supported; expected {IR_VERSION}."
            )
        value["nav"] = [NavNode.from_dict(node) for node in value["nav"]]
        return SiteRecord(**value)

    def write_page(self, page: PageRecord) -> None:
        self.write_json(self.pages / f"{page_key(page.url)}.json", asdict(page))

    def read_pages(self) -> list[PageRecord]:
        return [
            PageRecord(**json.loads(path.read_text()))
            for path in sorted(self.pages.glob("*.json"))
        ]

    def write_report(self, value: dict[str, object]) -> None:
        self.write_json(self.root / "report.json", value)
