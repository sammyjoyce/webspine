from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from docs2epub.build import chapter_name, xml_id
from docs2epub.model import PageRecord
from docs2epub.scrape import canonical_url, in_scope
from docs2epub.workspace import default_workspace, page_key, route_name


def evaluate(request: dict[str, object]) -> object:
    operation = request["operation"]
    arguments = request["arguments"]
    if not isinstance(arguments, list):
        raise TypeError("arguments must be a list")
    if operation == "canonical_url":
        return canonical_url(*arguments)
    if operation == "chapter_name":
        route, url = arguments
        return chapter_name(
            PageRecord(
                url=str(url),
                route=str(route),
                title="Generated page",
                language="en",
                html="",
                content_hash="",
                discovered_from=[],
            )
        )
    if operation == "default_workspace":
        return str(default_workspace(*arguments))
    if operation == "in_scope":
        return in_scope(*arguments)
    if operation == "page_key":
        return page_key(*arguments)
    if operation == "route_name":
        return route_name(*arguments)
    if operation == "xml_id":
        return xml_id(*arguments)
    raise ValueError(f"Unknown operation: {operation}")


def main() -> None:
    for line in sys.stdin:
        try:
            request = json.loads(line)
            response = {"result": evaluate(request)}
        except Exception as error:
            response = {"error": f"{type(error).__name__}: {error}"}
        print(json.dumps(response), flush=True)


if __name__ == "__main__":
    main()
