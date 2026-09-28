#include <gtest/gtest.h>
#include <hegel/hegel.h>

#include "core.hpp"

#include <openssl/sha.h>

#include <cstdio>
#include <regex>

namespace gs = hegel::generators;
using namespace webspine;

namespace {

const hegel::Settings settings{.test_cases = 2'000, .database = hegel::Database::disabled()};

// Independent SHA-256 oracle: OpenSSL's one-shot API and printf hex, not the
// EVP and hex helpers under test.
std::string sha256_oracle(const std::string& data) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest);
    char out[SHA256_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) std::snprintf(out + i * 2, 3, "%02x", digest[i]);
    return out;
}

std::string percent_encode(const std::string& value) {
    static const std::string unreserved =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.!~*'()";
    std::string out;
    for (unsigned char c : value) {
        if (unreserved.find(static_cast<char>(c)) != std::string::npos) {
            out += static_cast<char>(c);
        } else {
            char buffer[4];
            std::snprintf(buffer, sizeof buffer, "%%%02X", c);
            out += buffer;
        }
    }
    return out;
}

auto text() { return gs::text({.max_size = 200}); }
auto path_segment() { return gs::text({.min_size = 1, .max_size = 40}).map(percent_encode); }
// from_regex, not text({.alphabet}): hegel-cpp v0.13.0 passes an empty
// category list as a null pointer, which libhegel reads as "any character".
auto host() {
    return gs::from_regex("[a-z0-9]{1,20}").map([](std::string label) { return label + ".example"; });
}

bool matches(const std::string& value, const char* pattern) { return std::regex_match(value, std::regex(pattern)); }

}  // namespace

TEST(Properties, XmlIdIsNonemptyAndXmlSafe) {
    hegel::test([](hegel::TestCase& tc) {
        auto input = tc.draw("input", text());
        EXPECT_TRUE(matches(xml_id(input), "[A-Za-z_][A-Za-z0-9_.-]*")) << xml_id(input);
    }, settings);
}

TEST(Properties, XmlIdIsIdempotent) {
    hegel::test([](hegel::TestCase& tc) {
        auto once = xml_id(tc.draw("input", text()));
        EXPECT_EQ(xml_id(once), once);
    }, settings);
}

TEST(Properties, RouteNameIsPortableFilenameComponent) {
    hegel::test([](hegel::TestCase& tc) {
        auto result = route_name(tc.draw("route", text()));
        EXPECT_TRUE(matches(result, "[A-Za-z0-9._-]+")) << result;
        EXPECT_NE(result, ".");
        EXPECT_NE(result, "..");
    }, settings);
}

TEST(Properties, PageKeyIsSha256Prefix) {
    hegel::test([](hegel::TestCase& tc) {
        auto url = tc.draw("url", text());
        EXPECT_EQ(page_key(url), sha256_oracle(url).substr(0, 16));
    }, settings);
}

TEST(Properties, CanonicalUrlDropsQueryFragmentAndExtraSlashes) {
    hegel::test([](hegel::TestCase& tc) {
        auto domain = tc.draw("host", host());
        auto first = tc.draw("first", path_segment());
        auto second = tc.draw("second", path_segment());
        auto query = percent_encode(tc.draw("query", text()));
        auto fragment = percent_encode(tc.draw("fragment", text()));
        auto input = "https://" + domain + "//" + first + "///" + second + "//?q=" + query + "#" + fragment;
        EXPECT_EQ(canonical_url(input), "https://" + domain + "/" + first + "/" + second);
    }, settings);
}

TEST(Properties, InScopeAcceptsBaseDescendants) {
    hegel::test([](hegel::TestCase& tc) {
        auto base = "https://" + tc.draw("host", host()) + "/" + tc.draw("base", path_segment());
        EXPECT_TRUE(in_scope(base + "/" + tc.draw("child", path_segment()), base));
    }, settings);
}

TEST(Properties, InScopeRejectsSharedPrefixSiblings) {
    hegel::test([](hegel::TestCase& tc) {
        auto base = "https://" + tc.draw("host", host()) + "/" + tc.draw("base", path_segment());
        EXPECT_FALSE(in_scope(base + tc.draw("suffix", path_segment()), base));
    }, settings);
}

TEST(Properties, InScopeRejectsOtherHosts) {
    hegel::test([](hegel::TestCase& tc) {
        auto domain = tc.draw("host", host());
        auto other = tc.draw("other", host());
        tc.assume(domain != other);
        auto path = tc.draw("path", path_segment());
        EXPECT_FALSE(in_scope("https://" + other + "/" + path, "https://" + domain + "/" + path));
    }, settings);
}

TEST(Properties, ChapterNameIsPortableXhtmlFilenameWithUrlDigest) {
    hegel::test([](hegel::TestCase& tc) {
        auto route = tc.draw("route", text());
        auto url = tc.draw("url", gs::urls());
        auto result = chapter_name(route, url);
        EXPECT_TRUE(matches(result, "[A-Za-z0-9._-]+\\.xhtml")) << result;
        EXPECT_TRUE(result.ends_with("-" + sha256_oracle(url).substr(0, 8) + ".xhtml")) << result;
    }, settings);
}

TEST(Properties, DefaultWorkspaceStaysUnderWebspineRoot) {
    hegel::test([](hegel::TestCase& tc) {
        auto domain = tc.draw("host", host());
        auto first = tc.draw("first", path_segment());
        auto second = tc.draw("second", path_segment());
        EXPECT_EQ(default_workspace("https://" + domain + "/" + first + "/" + second).string(),
                  ".webspine/" + domain + "-" + first + "-" + second);
    }, settings);
}

TEST(Properties, UrlJoinOfAbsolutePathKeepsSchemeAndHost) {
    hegel::test([](hegel::TestCase& tc) {
        auto domain = tc.draw("host", host());
        auto base = "https://" + domain + "/" + tc.draw("base", path_segment()) + "/page";
        auto segment = tc.draw("target", path_segment());
        tc.assume(segment != "." && segment != "..");  // RFC 3986 section 5.2.4 removes dot segments.
        auto target = "/" + segment;
        EXPECT_EQ(url_join(base, target), "https://" + domain + target);
    }, settings);
}
