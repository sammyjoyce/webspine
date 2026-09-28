#include <gtest/gtest.h>

#include "build.hpp"
#include "core.hpp"
#include "media.hpp"
#include "workspace.hpp"

#include <unistd.h>
#include <vips/vips8>

#include <filesystem>

using namespace webspine;
namespace fs = std::filesystem;

TEST(Core, XmlIdIsStableAndXmlSafe) {
    EXPECT_EQ(xml_id("  42 / Score & Confidence "), "id-42-Score-Confidence");
    EXPECT_EQ(xml_id("already-safe"), "already-safe");
    EXPECT_EQ(xml_id(""), "id");
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
    PageRecord page{"https://docs.example/guide",
                    "/guide",
                    "Guide",
                    "en",
                    "<h1>Guide</h1><h4 id='a b' onclick='x()'>Step</h4><custom><p>Kept</p></custom>"
                    "<a href='/intro#Set Up'>Intro</a><a href='#a b'>Self</a><script>bad()</script>"
                    "<table><tr><td>Level</td></tr><tr><td>High</td></tr></table><img src='assets/x.webp'>",
                    "",
                    {},
                    {},
                    {}};
    auto result = clean_fragment(page, {{"https://docs.example/intro", "intro-1.xhtml"}}, {{"x.webp", "x.png"}});
    EXPECT_EQ(result,
              "<h2 id=\"a-b\">Step</h2><p>Kept</p><a href=\"intro-1.xhtml#Set-Up\">Intro</a>"
              "<a href=\"#a-b\">Self</a>bad()"
              "<table><tr><th scope=\"col\">Level</th></tr><tr><td>High</td></tr></table>"
              "<figure><img src=\"../images/x.png\" alt=\"Illustration\"/></figure>");
}

TEST(Core, CleanFragmentTurnsAlignedPreIntoTable) {
    PageRecord page{"https://docs.example/t",
                    "/t",
                    "T",
                    "en",
                    "<pre>Name  Score  Confidence\nAlpha  0.91  High\nBeta  0.72  Medium</pre>",
                    "",
                    {},
                    {},
                    {}};
    EXPECT_EQ(clean_fragment(page, {}, {}),
              "<table><tbody>"
              "<tr><th scope=\"col\">Name</th><th scope=\"col\">Score</th><th scope=\"col\">Confidence</th></tr>"
              "<tr><td>Alpha</td><td>0.91</td><td>High</td></tr>"
              "<tr><td>Beta</td><td>0.72</td><td>Medium</td></tr>"
              "</tbody></table>");
}

namespace {
std::string clean(const std::string& markup, const std::map<std::string, std::string>& files = {}) {
    PageRecord page{"https://docs.example/a", "/a", "A", "en", markup, "", {}, {}, {}};
    return clean_fragment(page, files, {});
}
}  // namespace

TEST(Core, XmlIdIsIdempotent) {
    for (std::string input : {"", "-", "1", "  ", "a b", "id-", "_x", "é"}) {
        auto once = xml_id(input);
        EXPECT_EQ(xml_id(once), once) << "input: '" << input << "'";
    }
}

TEST(Core, InlineImageStaysInline) {
    EXPECT_EQ(clean("<p>Icon <img src='i.png' alt='warn'> here.</p><img src='b.png' alt='big'>"),
              "<p>Icon <img src=\"i.png\" alt=\"warn\"/> here.</p>"
              "<figure><img src=\"b.png\" alt=\"big\"/></figure>");
}

TEST(Core, PreservesMeaningfulMarkup) {
    EXPECT_EQ(
        clean("<ol start='4' reversed><li value='9'>x</li></ol>"
              "<p lang='ja'><ruby>漢字<rp>(</rp><rt>かんじ</rt><rp>)</rp></ruby></p>"
              "<p dir='rtl'><bdi>إيان</bdi> <time datetime='2026-09-01'>then</time> a<wbr>b</p>"
              "<blockquote cite='https://q.example/'>q</blockquote><img src='i.png' alt='x' width='16' height='16'>"),
        "<ol start=\"4\" reversed=\"\"><li value=\"9\">x</li></ol>"
        "<p lang=\"ja\"><ruby>漢字<rp>(</rp><rt>かんじ</rt><rp>)</rp></ruby></p>"
        "<p dir=\"rtl\"><bdi>إيان</bdi> <time datetime=\"2026-09-01\">then</time> a<wbr/>b</p>"
        "<blockquote cite=\"https://q.example/\">q</blockquote>"
        "<figure><img src=\"i.png\" alt=\"x\" width=\"16\" height=\"16\"/></figure>");
}

TEST(Core, KeepsPresentationMathmlWithAltText) {
    EXPECT_EQ(clean("<p><math xmlns='http://www.w3.org/1998/Math/MathML'><msup><mi>e</mi><mn>2</mn></msup></math></p>"),
              "<p><math xmlns=\"http://www.w3.org/1998/Math/MathML\" "
              "alttext=\"e2\"><msup><mi>e</mi><mn>2</mn></msup></math></p>");
}

TEST(Core, EmptyFragmentLinksPointAtNothingInvalid) {
    auto result = clean("<p id='top'>x <a href='#'>Top</a></p>");
    EXPECT_EQ(result, "<p id=\"top\">x Top</p>");
}

TEST(Core, DanglingFragmentsArePruned) {
    auto content = clean(
        "<h2 id='here'>H</h2><a href='/b#there'>b</a> <a href='/b#gone'>c</a> "
        "<a href='#nope'>d</a> <a href='#here'>e</a> <a href='https://x.example/#k'>f</a>",
        {{"https://docs.example/b", "b.xhtml"}});
    EXPECT_EQ(prune_dangling_fragments(content, "a.xhtml", {{"a.xhtml", {"here"}}, {"b.xhtml", {"there"}}}),
              "<h2 id=\"here\">H</h2><a href=\"b.xhtml#there\">b</a> <a href=\"b.xhtml\">c</a> d "
              "<a href=\"#here\">e</a> <a href=\"https://x.example/#k\">f</a>");
}

TEST(Core, Base64MatchesRfc4648Vectors) {
    for (auto [plain, encoded] :
         std::vector<std::pair<std::string, std::string>>{{"", ""},
                                                          {"f", "Zg=="},
                                                          {"fo", "Zm8="},
                                                          {"foo", "Zm9v"},
                                                          {"foob", "Zm9vYg=="},
                                                          {"fooba", "Zm9vYmE="},
                                                          {"foobar", "Zm9vYmFy"},
                                                          {std::string("\xff\x00\xfe", 3), "/wD+"}}) {
        EXPECT_EQ(base64_encode(plain), encoded);
        EXPECT_EQ(base64_decode(encoded), plain);
    }
}

TEST(Core, Utf8ValidationRejectsLatin1AndOverlongForms) {
    EXPECT_TRUE(valid_utf8("Caf\xc3\xa9 \xf0\x9f\x93\x98"));
    EXPECT_FALSE(valid_utf8("Caf\xe9"));
    EXPECT_FALSE(valid_utf8("\xc0\xaf"));
    EXPECT_FALSE(valid_utf8("\xed\xa0\x80"));
    EXPECT_FALSE(valid_utf8("\xe2\x82"));
}

TEST(Core, CharsetDeclarationIsFoundInPrescanWindow) {
    EXPECT_TRUE(declares_charset("<html><head><META CHARSET=\"iso-8859-1\">"));
    EXPECT_TRUE(declares_charset("<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">"));
    EXPECT_FALSE(
        declares_charset("<html><head><meta name=\"viewport\" content=\"width=device-width\"><title>x</title>"));
    EXPECT_FALSE(declares_charset(std::string(1100, ' ') + "<meta charset=\"utf-8\">"));
}

namespace {

struct Cover {
    vips::VImage image;
    std::vector<unsigned char> pixels;  // row-major RGB, read once: per-pixel getpoint is slow

    std::vector<int> at(int x, int y) const {
        auto offset = (static_cast<size_t>(y) * image.width() + x) * 3;
        return {pixels[offset], pixels[offset + 1], pixels[offset + 2]};
    }
    // Rows in column x whose pixel is within 16 of color, sampled every 4 px between top and bottom.
    int rows_near(int x, std::vector<int> color, int top = 0, int bottom = 2560) const {
        int count = 0;
        for (int y = top; y < bottom; y += 4) {
            auto pixel = at(x, y);
            bool near = true;
            for (int i = 0; i < 3; ++i) near = near && std::abs(pixel[i] - color[i]) <= 16;
            count += near;
        }
        return count;
    }
};

// Decoded from bytes: libvips caches file loads by path, so reusing one temp path would return a stale cover.
Cover render(const CoverSpec& spec) {
    auto path = fs::temp_directory_path() / ("webspine-cover-" + std::to_string(::getpid()) + ".jpg");
    render_cover(spec, path);
    auto bytes = read_file(path);
    fs::remove(path);
    auto image = vips::VImage::new_from_buffer(bytes.data(), bytes.size(), "").copy_memory();
    size_t size = 0;
    auto* data = static_cast<unsigned char*>(image.write_to_memory(&size));
    std::vector<unsigned char> pixels(data, data + size);
    g_free(data);
    return {image, pixels};
}

fs::path solid_png(const std::string& name, int width, int height, std::vector<double> rgba) {
    auto path = fs::temp_directory_path() / name;
    vips::VImage::black(width, height).new_from_image(rgba).cast(VIPS_FORMAT_UCHAR).pngsave(path.c_str());
    return path;
}

}  // namespace

TEST(Cover, BareSpecStillRendersAPortraitCover) {
    auto cover = render({"Docs", "", "", "", {}, std::nullopt, std::nullopt});
    EXPECT_EQ(cover.image.width(), cover_width);
    EXPECT_EQ(cover.image.height(), cover_height);
    EXPECT_EQ(cover.rows_near(800, {0x24, 0x2b, 0x38}, 0, 280), 70) << "no theme colour or logo: neutral band";
}

TEST(Cover, PaleThemeColoursAreSkipped) {
    auto cover = render({"Docs", "", "", "", {"#ffffff", "#0b5fff"}, std::nullopt, std::nullopt});
    EXPECT_EQ(cover.rows_near(800, {0x0b, 0x5f, 0xff}, 0, 280), 70) << "the band uses the first dark theme colour";
}

TEST(Cover, AccentFallsBackToTheLogoColour) {
    auto logo = solid_png("webspine-red-logo.png", 200, 50, {0xb0, 0x10, 0x20, 255});
    auto cover = render({"Docs", "", "", "", {}, logo, std::nullopt});
    EXPECT_EQ(cover.rows_near(800, {0xb0, 0x10, 0x20}, 0, 280), 70);
}

TEST(Cover, LightLogoMovesOntoTheBand) {
    // A white logo on transparency is a dark-theme variant: on paper it would be invisible.
    auto logo = solid_png("webspine-white-logo.png", 400, 100, {255, 255, 255, 255});
    auto cover = render({"Docs", "", "", "", {"#102030"}, logo, std::nullopt});
    EXPECT_GT(cover.rows_near(250, {255, 255, 255}, 280, 720), 10) << "the logo sits inside the taller band";
    EXPECT_EQ(cover.rows_near(800, {0x10, 0x20, 0x30}, 600, 700), 25) << "the band grew to hold it";
}

TEST(Cover, LongTitleAndDescriptionStayAboveTheFooter) {
    std::string words;
    for (int i = 0; i < 400; ++i) words += "documentation ";
    auto cover = render({words.substr(0, 300), words, "", "", {}, std::nullopt, std::nullopt});
    // With no host or date, any dark pixel between the gap above the footer and the page bottom is overflow.
    constexpr int footer_rule = cover_height - 330;
    int overflow = 0;
    for (int x = 150; x < cover_width - 150; x += 7) {
        overflow += cover.rows_near(x, {0x5c, 0x5f, 0x66}, footer_rule - 100, cover_height);
        overflow += cover.rows_near(x, {0x1c, 0x1e, 0x22}, footer_rule - 100, cover_height);
    }
    EXPECT_EQ(overflow, 0);
    EXPECT_GT(cover.rows_near(300, {0x5c, 0x5f, 0x66}, 300, footer_rule) +
                  cover.rows_near(400, {0x5c, 0x5f, 0x66}, 300, footer_rule),
              0)
        << "a shortened description still appears";
}
