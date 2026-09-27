#include <gtest/gtest.h>

#include "build.hpp"
#include "core.hpp"

using namespace docs2epub;

TEST(Core, XmlIdIsStableAndXmlSafe) {
    EXPECT_EQ(xml_id("  42 / Score & Confidence "), "id-42-Score-Confidence");
    EXPECT_EQ(xml_id("already-safe"), "already-safe");
    EXPECT_EQ(xml_id(""), "id-");
}

TEST(Core, ScopeIsBoundedToHostAndPath) {
    std::string base = "https://example.com/docs";
    EXPECT_TRUE(in_scope("https://example.com/docs/guide", base));
    EXPECT_FALSE(in_scope("https://example.com/blog", base));
    EXPECT_FALSE(in_scope("https://example.com/docsearch", base));
    EXPECT_FALSE(in_scope("https://other.example/docs/guide", base));
    EXPECT_FALSE(in_scope("ftp://example.com/docs/guide", base));
    EXPECT_EQ(canonical_url("https://example.com/docs/guide/#part"), "https://example.com/docs/guide");
    EXPECT_EQ(canonical_url("http://127.0.0.1:8000"), "http://127.0.0.1:8000/");
}

TEST(Core, RouteNameAvoidsSpecialPathComponents) {
    EXPECT_EQ(route_name("."), "index");
    EXPECT_EQ(route_name(".."), "index");
    EXPECT_EQ(route_name("/guide/install step/"), "guide-install-step");
}

TEST(Core, UrlJoinFollowsRfc3986) {
    std::string base = "http://a/b/c/d;p?q";
    EXPECT_EQ(url_join(base, "g"), "http://a/b/c/g");
    EXPECT_EQ(url_join(base, "./g/"), "http://a/b/c/g/");
    EXPECT_EQ(url_join(base, "../../g"), "http://a/g");
    EXPECT_EQ(url_join(base, "//g"), "http://g");
    EXPECT_EQ(url_join(base, "#s"), "http://a/b/c/d;p?q#s");
    EXPECT_EQ(url_join(base, "?y"), "http://a/b/c/d;p?y");
    EXPECT_EQ(url_join(base, "https://x/y"), "https://x/y");
}

TEST(Core, HashesMatchKnownVectors) {
    EXPECT_EQ(sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(uuid5_url("http://python.org/"), "4c565f0d-3f5a-5890-b41b-20cf47701c5e");
}

TEST(Core, CleanFragmentSanitizesAndRewritesLinks) {
    PageRecord page{"https://docs.example/guide", "/guide", "Guide", "en",
                    "<h1>Guide</h1><h4 id='a b' onclick='x()'>Step</h4><custom><p>Kept</p></custom>"
                    "<a href='/intro#Set Up'>Intro</a><a href='#a b'>Self</a><script>bad()</script>"
                    "<table><tr><td>Level</td></tr><tr><td>High</td></tr></table><img src='assets/x.webp'>",
                    "", {}, {}, {}};
    auto result = clean_fragment(page, {{"https://docs.example/intro", "intro-1.xhtml"}}, {{"x.webp", "x.png"}});
    EXPECT_EQ(result,
              "<h2 id=\"a-b\">Step</h2><p>Kept</p><a href=\"intro-1.xhtml#Set-Up\">Intro</a>"
              "<a href=\"#a-b\">Self</a>bad()"
              "<table><tr><th scope=\"col\">Level</th></tr><tr><td>High</td></tr></table>"
              "<figure><img src=\"../images/x.png\" alt=\"Illustration\"/></figure>");
}

TEST(Core, CleanFragmentTurnsAlignedPreIntoTable) {
    PageRecord page{"https://docs.example/t", "/t", "T", "en",
                    "<pre>Name  Score  Confidence\nAlpha  0.91  High\nBeta  0.72  Medium</pre>", "", {}, {}, {}};
    EXPECT_EQ(clean_fragment(page, {}, {}),
              "<table><tbody>"
              "<tr><th scope=\"col\">Name</th><th scope=\"col\">Score</th><th scope=\"col\">Confidence</th></tr>"
              "<tr><td>Alpha</td><td>0.91</td><td>High</td></tr>"
              "<tr><td>Beta</td><td>0.72</td><td>Medium</td></tr>"
              "</tbody></table>");
}
