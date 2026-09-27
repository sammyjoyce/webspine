from docs2epub.build import xml_id
from docs2epub.scrape import canonical_url, in_scope


def test_xml_id_is_stable_and_xml_safe():
    assert xml_id("  42 / Score & Confidence ") == "id-42-Score-Confidence"
    assert xml_id("already-safe") == "already-safe"


def test_scope_is_bounded_to_host_and_path():
    base = "https://example.com/docs"
    assert in_scope("https://example.com/docs/guide", base)
    assert not in_scope("https://example.com/blog", base)
    assert not in_scope("https://other.example/docs/guide", base)
    assert (
        canonical_url("https://example.com/docs/guide/#part")
        == "https://example.com/docs/guide"
    )
