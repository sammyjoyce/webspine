#include <gtest/gtest.h>

#include "workspace.hpp"

#include <arpa/inet.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zip.h>
#include <vips/vips8>

#include <atomic>
#include <cstdio>
#include <regex>
#include <thread>

using namespace webspine;

namespace {

class StaticServer {
public:
    explicit StaticServer(fs::path root) : root_(std::move(root)) {
        socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 || listen(socket_, 64) != 0) {
            throw std::runtime_error("fixture server could not listen");
        }
        socklen_t size = sizeof address;
        getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~StaticServer() {
        stopping_ = true;
        shutdown(socket_, SHUT_RDWR);
        close(socket_);
        thread_.join();
    }

    int port() const { return port_; }

private:
    static std::string percent_decode(const std::string& value) {
        std::string out;
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '%' && i + 2 < value.size()) {
                out += static_cast<char>(std::stoi(value.substr(i + 1, 2), nullptr, 16));
                i += 2;
            } else {
                out += value[i];
            }
        }
        return out;
    }

    void serve() {
        while (!stopping_) {
            int client = accept(socket_, nullptr, nullptr);
            if (client < 0) continue;
            std::thread([this, client] { respond(client); }).detach();
        }
    }

    void respond(int client) {
        std::string request;
        char buffer[4096];
        while (request.find("\r\n\r\n") == std::string::npos) {
            ssize_t count = recv(client, buffer, sizeof buffer, 0);
            if (count <= 0) break;
            request.append(buffer, static_cast<size_t>(count));
        }
        if (!request.starts_with("GET ")) {
            close(client);
            return;
        }
        std::string target = request.substr(4, request.find(' ', 4) - 4);
        target = percent_decode(target.substr(0, target.find_first_of("?#")));
        if (target == "/moved") {
            std::string response =
                "HTTP/1.1 301 Moved Permanently\r\nLocation: /guide.html\r\nContent-Type: text/html\r\n"
                "Content-Length: 0\r\nConnection: close\r\n\r\n";
            send(client, response.data(), response.size(), MSG_NOSIGNAL);
            close(client);
            return;
        }
        if (target.ends_with('/')) target += "index.html";
        auto path = root_ / target.substr(1);
        std::string status = "200 OK";
        std::string body;
        std::string type = "application/octet-stream";
        if (target.find("..") == std::string::npos && fs::is_regular_file(path)) {
            body = read_file(path);
            auto extension = path.extension();
            if (extension == ".html") type = "text/html";
            if (extension == ".xml") type = "application/xml";
            if (extension == ".txt") type = "text/plain; charset=utf-8";
            if (extension == ".webp") type = "image/webp";
        } else {
            status = "404 Not Found";
            body = "missing";
        }
        std::string response = "HTTP/1.1 " + status + "\r\nContent-Type: " + type +
                               "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" +
                               body;
        send(client, response.data(), response.size(), MSG_NOSIGNAL);
        close(client);
    }

    fs::path root_;
    int socket_ = -1;
    int port_ = 0;
    std::atomic<bool> stopping_ = false;
    std::thread thread_;
};

std::pair<int, std::string> run_command(const std::string& command) {
    FILE* pipe = popen(command.c_str(), "r");
    std::string output;
    char buffer[4096];
    while (size_t count = fread(buffer, 1, sizeof buffer, pipe)) output.append(buffer, count);
    int status = pclose(pipe);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, output};
}

size_t count(const std::string& haystack, const std::string& needle) {
    size_t total = 0;
    for (auto at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + 1)) ++total;
    return total;
}

struct Entry {
    std::string name;
    std::string data;
    zip_uint16_t method = 0;
};

// Reads the raw local header of the first entry: EPUB 3.4 section 4.3.3 forbids
// an extra field on mimetype, and libzip's API does not expose local extras.
uint16_t first_local_extra_length(const std::string& archive) {
    return static_cast<uint16_t>(static_cast<unsigned char>(archive[28]) |
                                 (static_cast<unsigned char>(archive[29]) << 8));
}

class Book {
public:
    explicit Book(const fs::path& epub) : bytes(read_file(epub)) {
        int error = 0;
        zip_t* archive = zip_open(epub.c_str(), ZIP_RDONLY, &error);
        if (!archive) throw std::runtime_error("cannot open " + epub.string());
        for (zip_int64_t i = 0; i < zip_get_num_entries(archive, 0); ++i) {
            zip_stat_t stat{};
            zip_stat_index(archive, static_cast<zip_uint64_t>(i), 0, &stat);
            Entry entry{stat.name, std::string(stat.size, '\0'), stat.comp_method};
            zip_file_t* file = zip_fopen_index(archive, static_cast<zip_uint64_t>(i), 0);
            zip_fread(file, entry.data.data(), stat.size);
            zip_fclose(file);
            entries.push_back(std::move(entry));
        }
        zip_close(archive);
    }

    const std::string& read(const std::string& name) const {
        for (const auto& entry : entries) {
            if (entry.name == name) return entry.data;
        }
        throw std::runtime_error("missing entry " + name);
    }

    bool has(const std::string& name) const {
        return std::any_of(entries.begin(), entries.end(), [&](const auto& entry) { return entry.name == name; });
    }

    std::vector<std::string> names(const std::string& prefix) const {
        std::vector<std::string> out;
        for (const auto& entry : entries) {
            if (entry.name.starts_with(prefix)) out.push_back(entry.name);
        }
        return out;
    }

    std::string chapter(const std::string& stem) const {
        for (const auto& name : names("EPUB/text/")) {
            if (name.substr(10).starts_with(stem + "-")) return name;
        }
        throw std::runtime_error("no chapter for " + stem);
    }

    std::string bytes;
    std::vector<Entry> entries;
};

// XPath over a package or content document with the OPF, XHTML, DC and EPUB namespaces bound.
std::vector<std::string> xpath(const std::string& document, const std::string& expression) {
    xmlDocPtr doc =
        xmlReadMemory(document.data(), static_cast<int>(document.size()), nullptr, nullptr, XML_PARSE_NONET);
    if (!doc) throw std::runtime_error("document is not well-formed XML");
    xmlXPathContextPtr context = xmlXPathNewContext(doc);
    xmlXPathRegisterNs(context, BAD_CAST "opf", BAD_CAST "http://www.idpf.org/2007/opf");
    xmlXPathRegisterNs(context, BAD_CAST "dc", BAD_CAST "http://purl.org/dc/elements/1.1/");
    xmlXPathRegisterNs(context, BAD_CAST "h", BAD_CAST "http://www.w3.org/1999/xhtml");
    xmlXPathRegisterNs(context, BAD_CAST "epub", BAD_CAST "http://www.idpf.org/2007/ops");
    xmlXPathObjectPtr result = xmlXPathEvalExpression(BAD_CAST expression.c_str(), context);
    std::vector<std::string> out;
    if (result && result->nodesetval) {
        for (int i = 0; i < result->nodesetval->nodeNr; ++i) {
            xmlChar* value = xmlNodeGetContent(result->nodesetval->nodeTab[i]);
            out.emplace_back(reinterpret_cast<const char*>(value));
            xmlFree(value);
        }
    }
    xmlXPathFreeObject(result);
    xmlXPathFreeContext(context);
    xmlFreeDoc(doc);
    return out;
}

// Scrapes tests/fixtures/site once through the CLI and shares the result across tests.
class Fixture : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        char temp_template[] = "/tmp/webspine-e2e-XXXXXX";
        tmp = mkdtemp(temp_template);
        auto site = tmp / "site";
        fs::copy(WEBSPINE_FIXTURES "/site", site, fs::copy_options::recursive);
        if (VIPS_INIT("e2e") != 0) throw std::runtime_error("vips init failed");
        vips::VImage::black(640, 320)
            .new_from_image(std::vector<double>{0xd8, 0xe8, 0xf4})
            .cast(VIPS_FORMAT_UCHAR)
            .webpsave((site / "diagram.webp").c_str());
        StaticServer server(site);
        origin = "http://127.0.0.1:" + std::to_string(server.port());
        std::string sitemap = "<?xml version=\"1.0\"?><urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">";
        sitemap += "<url><loc>" + origin + "/moved</loc></url>";
        for (auto path : {"/", "/guide.html", "/reference/api%20notes.html", "/reference/i18n.html",
                          "/reference/formats.html", "/reference/latin1.html", "/reference/components.html"}) {
            sitemap += "<url><loc>" + origin + path + "</loc></url>";
        }
        write_file(site / "sitemap.xml", sitemap + "</urlset>");
        workspace = tmp / "work";
        epub = tmp / "fixture.epub";
        std::tie(exit_code, output) = run_command(std::string(WEBSPINE_BINARY) + " " + origin + "/ --workspace " +
                                                  workspace.string() + " --output " + epub.string() + " --json");
        if (fs::exists(epub)) book = std::make_unique<Book>(epub);
    }

    static void TearDownTestSuite() {
        book.reset();
        if (!::testing::UnitTest::GetInstance()->Failed()) fs::remove_all(tmp);
    }

    void SetUp() override { ASSERT_TRUE(book) << output; }

    static const std::string& opf() { return book->read("EPUB/package.opf"); }

    static inline fs::path tmp, workspace, epub;
    static inline std::string origin, output;
    static inline int exit_code = -1;
    static inline std::unique_ptr<Book> book;
};

}  // namespace

TEST_F(Fixture, CommandPassesEveryStage) {
    ASSERT_EQ(exit_code, 0) << output;
    auto report = json::parse(output);
    EXPECT_EQ(report["status"], "passed");
    std::vector<std::string> stages;
    for (const auto& stage : report["stages"]) stages.push_back(stage["stage"]);
    EXPECT_EQ(stages, (std::vector<std::string>{"scrape", "build", "validate"}));
    EXPECT_EQ(report["stages"][0]["counts"]["pages"], 7);
    EXPECT_EQ(json::parse(read_file(workspace / "checks" / "validation.json"))["status"], "passed");
}

// EPUB 3.4 section 4.3.3: mimetype is first, stored, exact ASCII, no extra field.
TEST_F(Fixture, MimetypeEntryFollowsOcf) {
    const auto& first = book->entries.front();
    EXPECT_EQ(first.name, "mimetype");
    EXPECT_EQ(first.data, "application/epub+zip");
    EXPECT_EQ(first.method, ZIP_CM_STORE);
    EXPECT_EQ(first_local_extra_length(book->bytes), 0);
}

// EPUB 3.4 section 5.6: package version, identifier, title, language, one dcterms:modified in UTC.
TEST_F(Fixture, PackageMetadataMeetsMinimum) {
    EXPECT_EQ(xpath(opf(), "/opf:package/@version"), (std::vector<std::string>{"3.0"}));
    auto identifier = xpath(opf(), "//dc:identifier[@id=/opf:package/@unique-identifier]");
    ASSERT_EQ(identifier.size(), 1u);
    EXPECT_TRUE(std::regex_match(
        identifier[0], std::regex("urn:uuid:[0-9a-f]{8}-[0-9a-f]{4}-5[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}")))
        << identifier[0];
    EXPECT_EQ(xpath(opf(), "//dc:title"), (std::vector<std::string>{"Fixture Docs"}));
    EXPECT_EQ(xpath(opf(), "//dc:language"), (std::vector<std::string>{"en"}));
    auto modified = xpath(opf(), "//opf:meta[@property='dcterms:modified' and not(@refines)]");
    ASSERT_EQ(modified.size(), 1u);
    EXPECT_TRUE(std::regex_match(modified[0], std::regex(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)"))) << modified[0];
}

// EPUB 3.4 section 5.7: every container file except mimetype, META-INF and the package is in the
// manifest, every manifest href exists, and exactly one item is the nav document.
TEST_F(Fixture, ManifestMatchesContainer) {
    std::set<std::string> listed;
    for (const auto& href : xpath(opf(), "//opf:manifest/opf:item/@href")) listed.insert("EPUB/" + href);
    std::set<std::string> present;
    for (const auto& entry : book->entries) {
        if (entry.name != "mimetype" && !entry.name.starts_with("META-INF/") && entry.name != "EPUB/package.opf" &&
            !entry.name.ends_with('/')) {
            present.insert(entry.name);
        }
    }
    EXPECT_EQ(listed, present);
    EXPECT_EQ(xpath(opf(), "//opf:item[contains(concat(' ', @properties, ' '), ' nav ')]").size(), 1u);
}

// EPUB 3.4 section 5.8: every chapter a spine item, at least one linear, and every
// document linked from the nav appears in the spine.
TEST_F(Fixture, SpineCoversNavigationTargets) {
    auto idrefs = xpath(opf(), "//opf:itemref/@idref");
    std::set<std::string> spine_hrefs;
    for (const auto& idref : idrefs) {
        for (const auto& href : xpath(opf(), "//opf:item[@id='" + idref + "']/@href")) spine_hrefs.insert(href);
    }
    EXPECT_GE(xpath(opf(), "//opf:itemref[not(@linear) or @linear='yes']").size(), 1u);
    auto toc = xpath(book->read("EPUB/nav.xhtml"), "//h:nav[@epub:type='toc']//h:a/@href");
    EXPECT_EQ(toc.size(), 7u);
    for (const auto& href : toc) EXPECT_TRUE(spine_hrefs.count(href)) << href;
    EXPECT_EQ(book->names("EPUB/text/").size(), 7u);
}

// EPUB 3.4 section 8.3: exactly one toc nav, and every link has a non-empty label.
TEST_F(Fixture, NavigationDocumentIsWellFormed) {
    const auto& nav = book->read("EPUB/nav.xhtml");
    EXPECT_EQ(xpath(nav, "//h:nav[@epub:type='toc']").size(), 1u);
    EXPECT_EQ(xpath(nav, "//h:nav//h:a[normalize-space(.)='']").size(), 0u);
    EXPECT_EQ(xpath(nav, "//h:nav[@epub:type='toc']//h:a"),
              (std::vector<std::string>{"Fixture Docs", "Guide", "API notes", "国際化 & \"Quotes\" <tags>",
                                        "Formats et caractères", "Page Latin-1",
                                        "Interface: AsyncTypeSafeClientConfigurationOptions"}));
}

// EPUB 3.4 section 4.2.3: file names avoid reserved characters and SPACE, and stay under 255 bytes.
TEST_F(Fixture, FileNamesArePortable) {
    for (const auto& entry : book->entries) {
        auto name = fs::path(entry.name).filename().string();
        EXPECT_LE(name.size(), 255u) << entry.name;
        EXPECT_EQ(name.find_first_of(" \"*:<>?\\|"), std::string::npos) << entry.name;
        EXPECT_FALSE(name.ends_with('.')) << entry.name;
    }
}

// EPUB 3.4 section 4.2.5: relative URLs resolve to container files, and fragments to existing IDs.
TEST_F(Fixture, CrossDocumentLinksResolveToIds) {
    auto intro_name = book->chapter("index");
    auto api_name = book->chapter("reference-api-20notes.html");
    auto guide_name = book->chapter("guide.html");
    const auto& intro = book->read(intro_name);
    auto hrefs = xpath(intro, "//h:a/@href");
    auto contains = [&](const std::string& value) {
        return std::find(hrefs.begin(), hrefs.end(), value) != hrefs.end();
    };
    EXPECT_TRUE(contains(guide_name.substr(10) + "#install")) << intro;
    EXPECT_TRUE(contains(api_name.substr(10) + "#Return-Values")) << intro;
    EXPECT_EQ(xpath(book->read(api_name), "//h:*[@id='Return-Values']").size(), 1u);
    EXPECT_EQ(xpath(book->read(guide_name), "//h:*[@id='install']").size(), 1u);
}

// EPUB 3.4 section 3.7: data URLs are not allowed in a element href.
TEST_F(Fixture, NoDataUrlHyperlinks) {
    for (const auto& name : book->names("EPUB/text/")) {
        EXPECT_EQ(xpath(book->read(name), "//h:a[starts-with(@href, 'data:')]").size(), 0u) << name;
    }
    EXPECT_NE(book->read(book->chapter("index")).find("open raw data"), std::string::npos);
}

// XHTML requirements: IDs are unique per document, and no script, handlers, or javascript: URLs survive.
TEST_F(Fixture, ContentIsSanitized) {
    const auto& guide = book->read(book->chapter("guide.html"));
    auto ids = xpath(guide, "//@id");
    EXPECT_EQ(std::set<std::string>(ids.begin(), ids.end()).size(), ids.size()) << guide;
    EXPECT_EQ(xpath(guide, "//h:*[@id='install-2']").size(), 1u) << guide;
    EXPECT_EQ(xpath(guide, "//h:a[.='Jump up']/@href"), (std::vector<std::string>{"#install"}));
    for (const auto& name : book->names("EPUB/text/")) {
        const auto& chapter = book->read(name);
        EXPECT_EQ(
            xpath(chapter, "//h:script | //@*[starts-with(name(), 'on')] | //h:a[starts-with(@href, 'javascript:')]")
                .size(),
            0u)
            << name;
        EXPECT_EQ(chapter.find("<!--"), std::string::npos) << name;
    }
    EXPECT_NE(guide.find("A scripted link"), std::string::npos);
}

// EPUB Accessibility 1.2 section 2.2: required schema.org metadata, including accessModeSufficient.
TEST_F(Fixture, AccessibilityMetadataIsPresent) {
    for (auto property : {"schema:accessModeSufficient", "schema:accessibilityFeature", "schema:accessibilityHazard",
                          "schema:accessMode", "schema:accessibilitySummary"}) {
        EXPECT_FALSE(xpath(opf(), std::string("//opf:meta[@property='") + property + "']").empty()) << property;
    }
}

// Every content document declares its language; per-page and inline languages survive.
TEST_F(Fixture, LanguageIsPreserved) {
    const auto& i18n = book->read(book->chapter("reference-i18n.html"));
    EXPECT_EQ(xpath(i18n, "/h:html/@lang"), (std::vector<std::string>{"ja"}));
    EXPECT_EQ(xpath(i18n, "//h:p[@lang='ar']").size(), 1u) << i18n;
    EXPECT_EQ(xpath(i18n, "//h:h1"), (std::vector<std::string>{"国際化 & \"Quotes\" <tags>"}));
    EXPECT_NE(i18n.find("📘 𝒳"), std::string::npos);
    for (const auto& name : book->names("EPUB/text/")) {
        EXPECT_EQ(xpath(book->read(name), "/h:html[@lang and @xml:lang]").size(), 1u) << name;
    }
}

// Accessible structure: heading outlines have no jumps, tables have headers, images have alt text.
TEST_F(Fixture, StructureIsAccessible) {
    const auto& api = book->read(book->chapter("reference-api-20notes.html"));
    EXPECT_EQ(xpath(api, "//h:*[@id='Return-Values']/self::h:h2").size(), 1u) << api;
    for (const auto& name : book->names("EPUB/text/")) {
        const auto& chapter = book->read(name);
        EXPECT_EQ(xpath(chapter, "//h:table[not(.//h:th)]").size(), 0u) << name;
        EXPECT_EQ(xpath(chapter, "//h:img[not(@alt) or @alt='']").size(), 0u) << name;
    }
    const auto& guide = book->read(book->chapter("guide.html"));
    EXPECT_EQ(xpath(guide, "//h:th[@scope='col']"),
              (std::vector<std::string>{"Name", "Score", "Confidence", "Level", "Meaning"}));
    EXPECT_EQ(count(guide, ">Guide</h"), 1u) << guide;
}

TEST_F(Fixture, CapturedContentSurvives) {
    const auto& intro = book->read(book->chapter("index"));
    EXPECT_NE(intro.find("This content must survive."), std::string::npos);
    EXPECT_NE(intro.find("pip install fixture"), std::string::npos);
    EXPECT_EQ(xpath(intro, "//h:img[@alt='Inline flow diagram']").size(), 1u);
    auto pngs = book->names("EPUB/images/");
    EXPECT_EQ(std::count_if(pngs.begin(), pngs.end(), [](const auto& n) { return n.ends_with(".png"); }), 2)
        << "the shared WEBP converts once and the inline SVG rasterizes";
    EXPECT_NE(book->read("EPUB/styles/book.css").find("hyphens: none"), std::string::npos);
    EXPECT_EQ(json::parse(read_file(workspace / "checks" / "coverage.json"))["coverage"], 1.0);
}

namespace {

struct Packing {
    bool mimetype_first = true;
    bool mimetype_stored = true;
};

void pack(const fs::path& source, const fs::path& epub, Packing packing) {
    int error = 0;
    zip_t* archive = zip_open(epub.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(source)) {
        if (entry.is_regular_file() && entry.path().filename() != "mimetype") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    if (packing.mimetype_first)
        files.insert(files.begin(), source / "mimetype");
    else
        files.push_back(source / "mimetype");
    for (const auto& path : files) {
        auto name = fs::relative(path, source).generic_string();
        auto index =
            zip_file_add(archive, name.c_str(), zip_source_file(archive, path.c_str(), 0, ZIP_LENGTH_TO_END), 0);
        bool store = name == "mimetype" && packing.mimetype_stored;
        zip_set_file_compression(archive, static_cast<zip_uint64_t>(index), store ? ZIP_CM_STORE : ZIP_CM_DEFLATE, 9);
    }
    zip_close(archive);
}

void replace_in(const fs::path& path, const std::string& from, const std::string& to) {
    auto text = read_file(path);
    auto at = text.find(from);
    if (at == std::string::npos) throw std::runtime_error("fixture text not found: " + from);
    write_file(path, text.replace(at, from.size(), to));
}

struct Mutation {
    const char* name;
    const char* expected_code;
    std::function<void(const fs::path&, Packing&)> apply;
};

class ValidatorMutation : public ::testing::TestWithParam<Mutation> {};

std::vector<std::string> finding_codes(const std::string& output) {
    std::vector<std::string> codes;
    for (const auto& finding : json::parse(output)["findings"]) codes.push_back(finding["code"]);
    return codes;
}

std::pair<int, std::string> validate_mutated(const Mutation* mutation, bool reflow = false) {
    char temp_template[] = "/tmp/webspine-min-XXXXXX";
    fs::path tmp = mkdtemp(temp_template);
    auto tree = tmp / "tree";
    fs::copy(WEBSPINE_FIXTURES "/minimal-epub", tree, fs::copy_options::recursive);
    Packing packing;
    if (mutation) mutation->apply(tree, packing);
    pack(tree, tmp / "book.epub", packing);
    auto result = run_command(std::string(WEBSPINE_BINARY) + " validate " + (tmp / "book.epub").string() +
                              (reflow ? " --json" : " --no-reflow --json"));
    fs::remove_all(tmp);
    return result;
}

}  // namespace

TEST(Validator, AcceptsMinimalConformingEpub) {
    auto [code, output] = validate_mutated(nullptr);
    EXPECT_EQ(code, 0) << output;
    EXPECT_EQ(finding_codes(output), std::vector<std::string>{}) << output;
}

TEST_P(ValidatorMutation, RejectsWithSpecificFinding) {
    auto [code, output] = validate_mutated(&GetParam());
    EXPECT_EQ(code, 1) << output;
    auto codes = finding_codes(output);
    EXPECT_NE(std::find(codes.begin(), codes.end(), GetParam().expected_code), codes.end()) << output;
}

INSTANTIATE_TEST_SUITE_P(
    Epub34, ValidatorMutation,
    ::testing::Values(
        Mutation{"MimetypeNotFirst", "EPUB_MIMETYPE_ORDER", [](auto&, Packing& p) { p.mimetype_first = false; }},
        Mutation{"MimetypeCompressed", "EPUB_MIMETYPE_COMPRESSED",
                 [](auto&, Packing& p) { p.mimetype_stored = false; }},
        Mutation{"MalformedXhtml", "XHTML_INVALID",
                 [](const fs::path& t, Packing&) { replace_in(t / "EPUB/text/two.xhtml", "</p>", "</div>"); }},
        Mutation{"MissingLanguage", "A11Y_LANGUAGE",
                 [](const fs::path& t, Packing&) {
                     replace_in(t / "EPUB/text/two.xhtml", " xml:lang=\"en\" lang=\"en\"", "");
                 }},
        Mutation{"ImageWithoutAlt", "A11Y_IMAGE_ALT",
                 [](const fs::path& t, Packing&) {
                     replace_in(t / "EPUB/text/two.xhtml", "</body>", "<p><img src=\"x.png\"/></p></body>");
                 }},
        Mutation{"TableWithoutHeaders", "A11Y_TABLE_HEADER",
                 [](const fs::path& t, Packing&) {
                     replace_in(t / "EPUB/text/one.xhtml", "<th scope=\"col\">Level</th>", "<td>Level</td>");
                 }},
        Mutation{"HeadingJump", "A11Y_HEADING_ORDER",
                 [](const fs::path& t, Packing&) {
                     replace_in(t / "EPUB/text/two.xhtml", "<h2 id=\"usage\">Usage</h2>",
                                "<h4 id=\"usage\">Usage</h4>");
                 }},
        Mutation{"BrokenInternalLink", "LINK_BROKEN_INTERNAL",
                 [](const fs::path& t, Packing&) { replace_in(t / "EPUB/text/two.xhtml", "one.xhtml", "zero.xhtml"); }},
        Mutation{"UnlistedResource", "EPUBCHECK_FAILED",
                 [](const fs::path& t, Packing&) {
                     replace_in(t / "EPUB/package.opf",
                                "<item id=\"two\" href=\"text/two.xhtml\" media-type=\"application/xhtml+xml\"/>", "");
                     replace_in(t / "EPUB/package.opf", "<itemref idref=\"two\"/>", "");
                 }},
        Mutation{"MissingModifiedDate", "EPUBCHECK_FAILED",
                 [](const fs::path& t, Packing&) {
                     replace_in(t / "EPUB/package.opf",
                                "<meta property=\"dcterms:modified\">2026-01-01T00:00:00Z</meta>", "");
                 }},
        Mutation{"DataUrlHyperlink", "EPUBCHECK_FAILED",
                 [](const fs::path& t, Packing&) {
                     replace_in(t / "EPUB/text/two.xhtml", "href=\"one.xhtml\"", "href=\"data:text/html,x\"");
                 }},
        Mutation{"UndefinedFragment", "EPUBCHECK_FAILED",
                 [](const fs::path& t, Packing&) { replace_in(t / "EPUB/text/one.xhtml", "#usage", "#nowhere"); }}),
    [](const auto& info) { return std::string(info.param.name); });

// Reflow names the overflowing word, not just the element type, so a failure says what to fix.
TEST(Validator, ReflowReportsTheOverflowingWord) {
    Mutation wide{"UnbreakableWord", "REFLOW_OVERFLOW_X", [](const fs::path& t, Packing&) {
                      replace_in(t / "EPUB/text/two.xhtml", "</body>",
                                 "<p style=\"white-space: nowrap\">see Supercalifragilistic_expialidocious_"
                                 "identifier_that_never_wraps_anywhere_at_all</p></body>");
                  }};
    auto [code, output] = validate_mutated(&wide, true);
    EXPECT_EQ(code, 1) << output;
    auto report = json::parse(output);
    bool named = false;
    for (const auto& finding : report["findings"]) {
        if (finding["code"] != "REFLOW_OVERFLOW_X") continue;
        auto details =
            json::parse(finding["message"].get<std::string>().substr(finding["message"].get<std::string>().find('{')));
        for (const auto& offender : details["offenders"]) {
            named = named || (offender["tag"] == "p" && offender["text"].get<std::string>().starts_with("Supercali"));
        }
    }
    EXPECT_TRUE(named) << output;
}

// A chapter without <body> is invalid, and validate must still return the report, not crash in reflow.
TEST(Validator, ReflowSurvivesAChapterWithoutBody) {
    Mutation bodiless{"NoBody", "EPUBCHECK_FAILED", [](const fs::path& t, Packing&) {
                          auto path = t / "EPUB/text/two.xhtml";
                          auto text = read_file(path);
                          write_file(path, text.substr(0, text.find("<body>")) + "</html>\n");
                      }};
    auto [code, output] = validate_mutated(&bodiless, true);
    EXPECT_EQ(code, 1) << output;
    auto codes = finding_codes(output);
    EXPECT_NE(std::find(codes.begin(), codes.end(), "EPUBCHECK_FAILED"), codes.end()) << output;
}

// HTML's encoding sniffing falls back to windows-1252 when neither the HTTP
// header nor a meta tag declares a charset; the scraper must still keep UTF-8 text.
TEST_F(Fixture, PagesWithoutDeclaredCharsetKeepUtf8) {
    const auto& formats = book->read(book->chapter("reference-formats.html"));
    EXPECT_EQ(xpath(formats, "//h:title"), (std::vector<std::string>{"Formats et caractères"})) << formats;
    EXPECT_NE(formats.find("Déjà vu"), std::string::npos) << formats;
    EXPECT_EQ(formats.find("Ã"), std::string::npos) << formats;
}

TEST_F(Fixture, RichInlineMarkupSurvives) {
    const auto& formats = book->read(book->chapter("reference-formats.html"));
    EXPECT_EQ(xpath(formats, "//h:p/h:img[@alt='avertissement']").size(), 1u) << "inline image stays in its paragraph";
    EXPECT_EQ(xpath(formats, "//h:ol/@start"), (std::vector<std::string>{"4"}));
    EXPECT_EQ(xpath(formats, "//h:li/@value"), (std::vector<std::string>{"9"}));
    EXPECT_EQ(xpath(formats, "//h:ruby/h:rt"), (std::vector<std::string>{"かんじ"}));
    EXPECT_EQ(xpath(formats, "//*[local-name()='math']/@alttext"), (std::vector<std::string>{"eiπ+1=0"}));
    auto chapter = book->chapter("reference-formats.html").substr(5);
    EXPECT_EQ(xpath(opf(), "//opf:item[@href='" + chapter + "']/@properties"), (std::vector<std::string>{"mathml"}));
}

TEST_F(Fixture, FragmentLinksTargetExistingIds) {
    const auto& formats = book->read(book->chapter("reference-formats.html"));
    EXPECT_EQ(xpath(formats, "//h:a[.='Haut de page']").size(), 0u) << formats;
    auto stale = xpath(formats, "//h:a[.='ancre périmée']/@href");
    ASSERT_EQ(stale.size(), 1u) << formats;
    EXPECT_EQ(stale[0], book->chapter("guide.html").substr(10));
}

// Mintlify-style markup: permalink icons and accordion titles put blocks inside headings and links.
TEST_F(Fixture, BlocksNeverNestInPhrasingContent) {
    const std::string phrasing =
        "self::h:a or self::h:span or self::h:strong or self::h:em or self::h:code or "
        "self::h:p or self::h:h1 or self::h:h2 or self::h:h3 or self::h:h4 or self::h:h5 or "
        "self::h:h6 or self::h:pre";
    const std::string block =
        "self::h:div or self::h:p or self::h:ul or self::h:ol or self::h:pre or "
        "self::h:table or self::h:section or self::h:figure or self::h:blockquote";
    for (const auto& name : book->names("EPUB/text/")) {
        const auto& chapter = book->read(name);
        EXPECT_EQ(xpath(chapter, "//*[" + block + "][ancestor::*[" + phrasing + "]]").size(), 0u) << name;
    }
    const auto& components = book->read(book->chapter("reference-components.html"));
    EXPECT_EQ(xpath(components, "//h:h2[@id='setup']"), (std::vector<std::string>{"Setup"})) << components;
    EXPECT_EQ(xpath(components, "//h:h2[@id='setup']//h:a").size(), 0u) << "the permalink icon is dropped";
    EXPECT_EQ(xpath(components, "//h:h3[not(parent::*[@id])]"), (std::vector<std::string>{"Show properties"}))
        << components;
    EXPECT_EQ(xpath(components, "//h:a[@href='" + book->chapter("guide.html").substr(10) + "']"),
              (std::vector<std::string>{"Guide cardOpen the guide from a card link."}))
        << "card links keep their text";
    EXPECT_EQ(xpath(components, "//h:pre/h:code"),
              (std::vector<std::string>{"npm install fixture-sdk", "pnpm add fixture-sdk", "fixture install"}))
        << "whitespace-only highlighter spans survive";
    for (const auto& target : {"legacy-anchor", "wrapped-list"}) {
        EXPECT_EQ(xpath(components, std::string("//*[@id='") + target + "']").size(), 1u) << target;
        EXPECT_EQ(xpath(components, std::string("//h:a[@href='#") + target + "']").size(), 1u)
            << target << " stays a working link target";
    }
}

// Each panel is labelled with its tab name, so the tab strip itself is dropped instead of left as a bare list.
TEST_F(Fixture, TabStripsBecomePanelLabels) {
    const auto& components = book->read(book->chapter("reference-components.html"));
    EXPECT_EQ(xpath(components, "//h:li[.='npm']").size(), 0u) << components;
    EXPECT_EQ(xpath(components, "//*[@id='panel-npm']/h:h3"), (std::vector<std::string>{"npm"})) << components;
    EXPECT_EQ(xpath(components, "//*[@id='panel-pnpm']/h:h3"), (std::vector<std::string>{"pnpm"})) << components;
    EXPECT_EQ(xpath(components, "//*[@id='panel-pnpm']//h:code"), (std::vector<std::string>{"pnpm add fixture-sdk"}));
}

// Coverage compares prose, so Markdown links and inline HTML in llms-full.txt must not count as missing text.
TEST_F(Fixture, CoverageIgnoresSourceMarkup) {
    auto missing = json::parse(read_file(workspace / "checks" / "coverage.json"))["missing"];
    EXPECT_EQ(missing, json::array()) << missing.dump(2);
}

// Long identifiers in headings, code, and links wrap instead of widening the page.
TEST_F(Fixture, LongTokensWrap) {
    auto reflow = json::parse(read_file(workspace / "checks" / "reflow.json"));
    for (const auto& result : reflow) {
        if (result["file"].get<std::string>().starts_with("reference-components.html")) {
            EXPECT_EQ(result["offenders"], json::array()) << result.dump();
            EXPECT_LE(result["documentWidth"], result["viewportWidth"]) << result.dump();
        }
    }
}

TEST_F(Fixture, DeclaredLegacyCharsetIsHonoured) {
    const auto& latin = book->read(book->chapter("reference-latin1.html"));
    EXPECT_NE(latin.find("café, déjà, naïve"), std::string::npos) << latin;
}

// With only stdin, stdout and stderr open, pipe2 returns fds 3 and 4, the numbers
// Chromium's --remote-debugging-pipe expects. The browser must still receive both.
TEST(Browser, StartsWhenPipesLandOnDebuggingFds) {
    auto [code, output] = run_command("env -i HOME=/tmp PATH=\"$PATH\" " WEBSPINE_BINARY
                                      " scrape about:blank --workspace /tmp/webspine-fd-probe --json 3<&- 4<&- 5<&-");
    fs::remove_all("/tmp/webspine-fd-probe");
    EXPECT_EQ(output.find("Chromium exited"), std::string::npos) << output;
}
