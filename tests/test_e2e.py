from __future__ import annotations

import json
import subprocess
import sys
import threading
import zipfile
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from typing import TYPE_CHECKING

from lxml import etree
from PIL import Image

if TYPE_CHECKING:
    from pathlib import Path


def test_one_command_builds_and_validates_fixture(tmp_path: Path):
    site = tmp_path / "site"
    site.mkdir()
    Image.new("RGB", (640, 320), "#d8e8f4").save(site / "diagram.webp", "WEBP")
    (site / "index.html").write_text("""<!doctype html>
<html lang="en"><head><title>Fixture Docs</title></head><body>
<aside><nav aria-label="Documentation"><a href="/">Introduction</a><a href="/guide.html">Guide</a></nav></aside>
<main><article><h1>Fixture Docs</h1><p>This rendered documentation fixture preserves prose through the complete publication pipeline.</p>
<details><summary>Hidden details</summary><p>This content must survive.</p></details>
<div role="tablist"><button role="tab" aria-controls="python">Python</button></div>
<section id="python" hidden="hidden"><p>pip install fixture</p></section>
<img src="/diagram.webp" alt="Fixture architecture"/>
<svg aria-label="Inline flow diagram" viewBox="0 0 100 40"><rect width="100" height="40" fill="white"/><text x="10" y="25">Flow</text></svg>
<a href="/guide.html#install">Install it</a></article></main>
<script>document.querySelector('#python').hidden = true;</script>
</body></html>""")
    (site / "guide.html").write_text("""<!doctype html>
<html lang="en"><head><title>Guide</title></head><body>
<aside><nav aria-label="Documentation"><a href="/">Introduction</a><a href="/guide.html">Guide</a></nav></aside>
<main><article><h1>Guide</h1><h3 id="install">Install</h3>
<pre><code>fixture --a-very-long-option-name that wraps without clipping</code></pre>
<pre>Name        Score       Confidence
Alpha       0.91        High
Beta        0.72        Medium</pre>
<table><tr><td>Level</td><td>Meaning</td></tr><tr><td>High</td><td>Ready to ship</td></tr></table>
</article></main></body></html>""")

    server = ThreadingHTTPServer(
        ("127.0.0.1", 0), lambda *args: SimpleHTTPRequestHandler(*args, directory=site)
    )
    port = server.server_address[1]
    (
        site / "sitemap.xml"
    ).write_text(f"""<?xml version="1.0"?><urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">
<url><loc>http://127.0.0.1:{port}/</loc></url><url><loc>http://127.0.0.1:{port}/guide.html</loc></url></urlset>""")
    (site / "llms-full.txt").write_text(
        "This rendered documentation fixture preserves prose through the complete publication pipeline.\n"
    )
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    workspace = tmp_path / "work"
    epub = tmp_path / "fixture.epub"
    try:
        process = subprocess.run(
            [
                sys.executable,
                "-m",
                "docs2epub.cli",
                f"http://127.0.0.1:{port}/",
                "--workspace",
                str(workspace),
                "--output",
                str(epub),
                "--json",
            ],
            text=True,
            capture_output=True,
            check=False,
        )
    finally:
        server.shutdown()
        thread.join()

    assert process.returncode == 0, process.stdout + process.stderr
    report = json.loads(process.stdout)
    assert report["status"] == "passed"
    assert [stage["stage"] for stage in report["stages"]] == [
        "scrape",
        "build",
        "validate",
    ]
    assert epub.exists()

    with zipfile.ZipFile(epub) as archive:
        assert archive.namelist()[0] == "mimetype"
        assert archive.read("mimetype") == b"application/epub+zip"
        chapters = [
            name for name in archive.namelist() if name.startswith("EPUB/text/")
        ]
        assert len(chapters) == 2
        guide = next(name for name in chapters if "guide" in name)
        guide_xml = archive.read(guide)
        etree.fromstring(guide_xml)
        assert b'<th scope="col">Level</th>' in guide_xml
        assert b'<th scope="col">Name</th>' in guide_xml
        assert guide_xml.count(b">Guide</h") == 1
        assert b"hyphens: none" in archive.read("EPUB/styles/book.css")

    site_record = json.loads((workspace / "site.json").read_text())
    assert len(site_record["pages"]) == 2
    validation = json.loads((workspace / "checks" / "validation.json").read_text())
    assert validation["status"] == "passed"
    coverage = json.loads((workspace / "checks" / "coverage.json").read_text())
    assert coverage["coverage"] == 1.0
