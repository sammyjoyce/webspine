from __future__ import annotations

import argparse
import json
import shutil
import sys
from dataclasses import asdict
from pathlib import Path

from . import __version__
from .build import build
from .model import Finding, Report, StageResult
from .scrape import ScrapeOptions, scrape
from .validate import validate
from .workspace import Workspace, default_workspace


def _workspace(value: str | None, url: str | None = None) -> Path:
    if value:
        return Path(value)
    if url:
        return default_workspace(url)
    raise ValueError("--workspace is required for this command.")


def _emit(value: object, json_output: bool) -> None:
    if json_output:
        print(json.dumps(value, indent=2, default=str))
    else:
        if isinstance(value, dict):
            for key, item in value.items():
                print(f"{key}: {item}")
        else:
            print(value)


def _scrape_stage(args: argparse.Namespace, workspace: Path) -> StageResult:
    site = scrape(
        ScrapeOptions(
            url=args.url,
            workspace=workspace,
            title=getattr(args, "title", None),
            language=getattr(args, "language", None),
            content_selector=getattr(args, "content_selector", None),
            nav_selector=getattr(args, "nav_selector", None),
            max_pages=getattr(args, "max_pages", 500),
            force=getattr(args, "force", False),
        )
    )
    return StageResult("scrape", "passed", counts={"pages": len(site.pages)})


def _run(args: argparse.Namespace) -> int:
    workspace = _workspace(args.workspace, args.url)
    stages: list[StageResult] = []
    epub: Path | None = None
    active_stage = "scrape"
    try:
        stages.append(_scrape_stage(args, workspace))
        active_stage = "build"
        built = build(workspace, Path(args.output) if args.output else None)
        epub = built.epub
        stages.append(
            StageResult(
                "build",
                "passed",
                counts={"chapters": built.chapters, "assets": built.assets},
            )
        )
        active_stage = "validate"
        stages.append(validate(epub, workspace, reflow=not args.no_reflow))
    except Exception as error:
        stages.append(
            StageResult(
                active_stage,
                "failed",
                findings=[Finding("COMMAND_FAILED", "error", str(error), active_stage)],
            )
        )
    passed = all(stage.status == "passed" for stage in stages)
    report = Report(
        "run",
        "passed" if passed else "failed",
        str(workspace.resolve()),
        str(epub) if epub else None,
        stages,
    )
    Workspace(workspace).create()
    Workspace(workspace).write_report(report.to_dict())
    _emit(report.to_dict(), args.json)
    return 0 if passed else 1


def _doctor(args: argparse.Namespace) -> int:
    tools = {
        "python": sys.executable,
        "epubcheck": shutil.which("epubcheck"),
        "playwright_browsers": bool(
            __import__("os").environ.get("PLAYWRIGHT_BROWSERS_PATH")
        ),
        "version": __version__,
    }
    ok = bool(tools["epubcheck"] and tools["playwright_browsers"])
    tools["status"] = "passed" if ok else "failed"
    _emit(tools, args.json)
    return 0 if ok else 3


def _inspect(args: argparse.Namespace) -> int:
    workspace = Workspace(_workspace(args.workspace))
    if args.subject == "report":
        value = json.loads((workspace.root / "report.json").read_text())
    elif args.subject == "pages":
        value = [asdict(page) for page in workspace.read_pages()]
    else:
        value = asdict(workspace.read_site())
    _emit(value, args.json)
    return 0


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(
        prog="docs2epub",
        description="Turn a rendered documentation site into a validated EPUB.",
    )
    root.add_argument("--version", action="version", version=__version__)
    commands = root.add_subparsers(dest="command", required=True)

    def scrape_flags(command: argparse.ArgumentParser) -> None:
        command.add_argument("url")
        command.add_argument("--workspace")
        command.add_argument("--title")
        command.add_argument("--language")
        command.add_argument("--content-selector")
        command.add_argument("--nav-selector")
        command.add_argument("--max-pages", type=int, default=500)
        command.add_argument("--force", action="store_true")
        command.add_argument("--json", action="store_true")

    run_parser = commands.add_parser(
        "run", help="Scrape, build, and validate a documentation site."
    )
    scrape_flags(run_parser)
    run_parser.add_argument("-o", "--output")
    run_parser.add_argument("--no-reflow", action="store_true")

    scrape_parser = commands.add_parser(
        "scrape", help="Capture rendered pages into a rerunnable workspace."
    )
    scrape_flags(scrape_parser)

    build_parser = commands.add_parser(
        "build", help="Build an EPUB from a captured workspace without network access."
    )
    build_parser.add_argument("--workspace", required=True)
    build_parser.add_argument("-o", "--output")
    build_parser.add_argument("--json", action="store_true")

    validate_parser = commands.add_parser("validate", help="Validate a packaged EPUB.")
    validate_parser.add_argument("epub")
    validate_parser.add_argument("--workspace")
    validate_parser.add_argument("--no-reflow", action="store_true")
    validate_parser.add_argument("--json", action="store_true")

    inspect_parser = commands.add_parser(
        "inspect", help="Inspect a workspace without changing it."
    )
    inspect_parser.add_argument(
        "subject", choices=("site", "pages", "report"), default="site", nargs="?"
    )
    inspect_parser.add_argument("--workspace", required=True)
    inspect_parser.add_argument("--json", action="store_true")

    doctor_parser = commands.add_parser(
        "doctor", help="Check the local runtime and validators."
    )
    doctor_parser.add_argument("--json", action="store_true")
    return root


def main(argv: list[str] | None = None) -> None:
    values = list(sys.argv[1:] if argv is None else argv)
    if values and values[0].startswith(("http://", "https://")):
        values.insert(0, "run")
    args = parser().parse_args(values)
    if args.command == "run":
        code = _run(args)
    elif args.command == "scrape":
        workspace = _workspace(args.workspace, args.url)
        try:
            result = _scrape_stage(args, workspace)
            _emit(asdict(result), args.json)
            code = 0
        except Exception as error:
            _emit({"status": "failed", "error": str(error)}, args.json)
            code = 1
    elif args.command == "build":
        built = build(Path(args.workspace), Path(args.output) if args.output else None)
        _emit(
            {
                "status": "passed",
                "epub": str(built.epub),
                "chapters": built.chapters,
                "assets": built.assets,
            },
            args.json,
        )
        code = 0
    elif args.command == "validate":
        result = validate(
            Path(args.epub),
            Path(args.workspace) if args.workspace else None,
            not args.no_reflow,
        )
        _emit(asdict(result), args.json)
        code = 0 if result.status == "passed" else 1
    elif args.command == "inspect":
        code = _inspect(args)
    else:
        code = _doctor(args)
    raise SystemExit(code)


if __name__ == "__main__":
    main()
