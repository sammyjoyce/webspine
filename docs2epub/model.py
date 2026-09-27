from __future__ import annotations

from dataclasses import asdict, dataclass, field
from typing import Any, Literal

IR_VERSION = 1


@dataclass
class NavNode:
    title: str
    url: str | None = None
    children: list[NavNode] = field(default_factory=list)

    @classmethod
    def from_dict(cls, value: dict[str, Any]) -> NavNode:
        return cls(
            title=value["title"],
            url=value.get("url"),
            children=[cls.from_dict(child) for child in value.get("children", [])],
        )


@dataclass
class PageRecord:
    url: str
    route: str
    title: str
    language: str
    html: str
    content_hash: str
    discovered_from: list[str]
    assets: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)


@dataclass
class SiteRecord:
    base_url: str
    title: str
    language: str
    adapter: str
    ir_version: int
    sitemap_urls: list[str]
    nav: list[NavNode]
    pages: list[str]


Severity = Literal["error", "warning", "info"]


@dataclass
class Finding:
    code: str
    severity: Severity
    message: str
    stage: str
    url: str | None = None
    file: str | None = None
    hint: str | None = None


@dataclass
class StageResult:
    stage: str
    status: Literal["passed", "failed", "skipped"]
    counts: dict[str, int] = field(default_factory=dict)
    findings: list[Finding] = field(default_factory=list)


@dataclass
class Report:
    command: str
    status: Literal["passed", "failed"]
    workspace: str
    epub: str | None
    stages: list[StageResult]
    format_version: int = 1

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)
