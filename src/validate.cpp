#include "validate.hpp"

#include "browser.hpp"
#include "core.hpp"
#include "html.hpp"
#include "workspace.hpp"

#include <libxml/parser.h>
#include <sys/wait.h>
#include <zip.h>

#include <algorithm>
#include <cstdio>
#include <format>
#include <regex>
#include <set>
#include <sstream>

namespace docs2epub {
namespace {

std::vector<fs::path> sorted_files(const fs::path& root, const std::string& extension, bool recursive) {
    std::vector<fs::path> out;
    if (!fs::exists(root)) return out;
    auto consider = [&](const fs::directory_entry& entry) {
        if (entry.is_regular_file() && entry.path().extension() == extension) out.push_back(entry.path());
    };
    if (recursive) {
        for (const auto& entry : fs::recursive_directory_iterator(root)) consider(entry);
    } else {
        for (const auto& entry : fs::directory_iterator(root)) consider(entry);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Finding> extract(const fs::path& epub, const fs::path& destination) {
    fs::remove_all(destination);
    fs::create_directories(destination);
    int error = 0;
    zip_t* archive = zip_open(epub.c_str(), ZIP_RDONLY, &error);
    if (!archive) throw std::runtime_error("Could not open EPUB " + epub.string());
    std::vector<Finding> findings;
    zip_stat_t first{};
    if (zip_get_num_entries(archive, 0) == 0 || zip_stat_index(archive, 0, 0, &first) != 0 ||
        std::string(first.name) != "mimetype") {
        findings.push_back(error_finding("EPUB_MIMETYPE_ORDER", "mimetype is not the first ZIP entry."));
    } else if (first.comp_method != ZIP_CM_STORE) {
        findings.push_back(error_finding("EPUB_MIMETYPE_COMPRESSED", "mimetype must be stored without compression."));
    }
    auto root = fs::weakly_canonical(destination);
    for (zip_int64_t index = 0; index < zip_get_num_entries(archive, 0); ++index) {
        zip_stat_t stat{};
        zip_stat_index(archive, static_cast<zip_uint64_t>(index), 0, &stat);
        std::string name = stat.name;
        auto target = fs::weakly_canonical(root / name);
        if (!target.string().starts_with(root.string() + "/")) {
            zip_close(archive);
            throw std::runtime_error("EPUB entry escapes the extraction directory: " + name);
        }
        if (name.ends_with('/')) {
            fs::create_directories(target);
            continue;
        }
        zip_file_t* file = zip_fopen_index(archive, static_cast<zip_uint64_t>(index), 0);
        std::string data(stat.size, '\0');
        zip_int64_t read = file ? zip_fread(file, data.data(), stat.size) : -1;
        if (file) zip_fclose(file);
        if (read < 0) {
            zip_close(archive);
            throw std::runtime_error("Could not read EPUB entry " + name);
        }
        write_file(target, data);
    }
    zip_close(archive);
    return findings;
}

std::vector<Finding> content_findings(const fs::path& extracted) {
    std::vector<Finding> findings;
    auto xhtml_files = sorted_files(extracted, ".xhtml", true);
    std::set<fs::path> known;
    for (const auto& path : xhtml_files) known.insert(fs::weakly_canonical(path));
    for (const auto& path : xhtml_files) {
        auto file = path.string();
        xmlResetLastError();
        xmlDocPtr doc = xmlReadFile(path.c_str(), nullptr, XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
        if (!doc) {
            const xmlError* error = xmlGetLastError();
            std::string message = error && error->message ? strip_whitespace(error->message) : "XML parse error";
            findings.push_back(error_finding("XHTML_INVALID", message, "validate", file));
            continue;
        }
        auto document = reinterpret_cast<xmlNodePtr>(doc);
        auto by_name = [](std::string tag) { return [tag](xmlNodePtr node) { return html::name(node) == tag; }; };
        xmlNodePtr root = xmlDocGetRootElement(doc);
        if (!root || html::name(root) != "html" ||
            (html::attr(root, "lang").value_or("").empty() && html::attr(root, "xml:lang").value_or("").empty() &&
             !xmlNodeGetLang(root))) {
            findings.push_back(error_finding("A11Y_LANGUAGE", "The XHTML document has no language.", "validate", file));
        }
        for (auto image : html::elements(document, by_name("img"))) {
            if (html::attr(image, "alt").value_or("").empty()) {
                findings.push_back(error_finding("A11Y_IMAGE_ALT", "An image has no alt text.", "validate", file));
            }
        }
        for (auto table : html::elements(document, by_name("table"))) {
            if (!html::first_element(table, by_name("th"))) {
                findings.push_back(error_finding("A11Y_TABLE_HEADER", "A table has no header cells.", "validate", file));
            }
        }
        int previous = 0;
        for (auto heading : html::elements(document, html::is_heading)) {
            int level = html::heading_level(heading);
            if (previous && level > previous + 1) {
                findings.push_back(error_finding(
                    "A11Y_HEADING_ORDER", std::format("Heading level jumps from h{} to h{}.", previous, level),
                    "validate", file));
            }
            previous = level;
        }
        for (auto link : html::elements(document, by_name("a"))) {
            auto href = html::attr(link, "href");
            if (!href || starts_with_any(*href, {"http://", "https://", "mailto:", "tel:", "#"})) continue;
            auto target = url_defrag(*href).first;
            auto resolved = fs::weakly_canonical(path.parent_path() / target);
            if (resolved.extension() == ".xhtml" && !known.count(resolved)) {
                findings.push_back(error_finding("LINK_BROKEN_INTERNAL", "Internal link target does not exist: " + *href,
                                                 "validate", file));
            }
        }
        xmlFreeDoc(doc);
    }
    return findings;
}

std::string shell_quote(const std::string& value) {
    std::string out = "'";
    for (char c : value) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

std::vector<Finding> epubcheck(const fs::path& epub, const fs::path& checks) {
    std::string command = "epubcheck " + shell_quote(epub.string()) + " -j " +
                          shell_quote((checks / "epubcheck.json").string()) + " 2>&1";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return {error_finding("ENV_EPUBCHECK_MISSING", "epubcheck could not be started.")};
    std::string output;
    char buffer[4096];
    while (size_t count = fread(buffer, 1, sizeof buffer, pipe)) output.append(buffer, count);
    int status = pclose(pipe);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    write_file(checks / "epubcheck.txt", output);
    if (code == 127) {
        Finding finding = error_finding("ENV_EPUBCHECK_MISSING", "epubcheck is not on PATH.");
        finding.hint = "Run through nix run or nix develop.";
        return {finding};
    }
    if (code == 0) return {};
    std::string message = strip_whitespace(output);
    if (message.empty()) message = "EPUBCheck failed.";
    if (message.size() > 4000) message = message.substr(message.size() - 4000);
    return {error_finding("EPUBCHECK_FAILED", message, "validate", epub.string())};
}

constexpr std::string_view overflow_js = R"JS((() => ({
  documentWidth: document.documentElement.scrollWidth,
  viewportWidth: document.documentElement.clientWidth,
  offenders: [...document.querySelectorAll('pre,table,img,svg')]
    .filter((node) => node.getBoundingClientRect().right > document.documentElement.clientWidth + 1)
    .slice(0, 10)
    .map((node) => node.tagName.toLowerCase())
}))())JS";

std::vector<Finding> reflow(const fs::path& extracted, const fs::path& checks) {
    std::vector<Finding> findings;
    json results = json::array();
    auto files = sorted_files(extracted / "EPUB" / "text", ".xhtml", false);
    Browser browser;
    for (auto [width, scale] : {std::pair{320, 1.0}, {390, 1.5}, {768, 2.0}}) {
        browser.set_viewport(width, 900);
        for (const auto& path : files) {
            browser.navigate("file://" + fs::weakly_canonical(path).string(), false);
            browser.evaluate(std::format("document.documentElement.style.fontSize = '{}%'", scale * 100));
            json overflow = browser.evaluate(overflow_js);
            json result = {{"file", path.filename().string()}, {"width", width}, {"scale", scale}};
            result.update(overflow);
            results.push_back(result);
            if (overflow["documentWidth"].get<int>() > overflow["viewportWidth"].get<int>() + 1 ||
                !overflow["offenders"].empty()) {
                findings.push_back(error_finding(
                    "REFLOW_OVERFLOW_X",
                    std::format("Horizontal overflow at {}px and {:.1f}x text: {}", width, scale, overflow.dump()),
                    "validate", path.string()));
            }
        }
    }
    write_file(checks / "reflow.json", results.dump(2) + "\n");
    return findings;
}

std::string lowercase_words(std::string_view text) {
    std::string out;
    bool pending_space = false;
    for (unsigned char c : text) {
        if (std::isspace(c)) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) out += ' ';
        pending_space = false;
        out += static_cast<char>(std::tolower(c));
    }
    return out;
}

std::vector<Finding> coverage(const Workspace& workspace) {
    std::vector<Finding> findings;
    SiteRecord site = workspace.read_site();
    std::set<std::string> captured;
    for (const auto& page : workspace.read_pages()) captured.insert(page.url);
    std::set<std::string> expected(site.sitemap_urls.begin(), site.sitemap_urls.end());
    for (const auto& node : site.nav) {
        if (node.url) expected.insert(*node.url);
    }
    for (const auto& url : expected) {
        if (!captured.count(url)) {
            Finding finding = error_finding("COVERAGE_MISSING_PAGE", "A discovered page was not captured.");
            finding.url = url;
            findings.push_back(finding);
        }
    }
    auto source = workspace.root / "source" / "llms-full.txt";
    if (!fs::exists(source)) return findings;

    static const std::regex bullet(R"(^[*+-]\s+)");
    static const std::regex markup(R"([*_`\\])");
    std::vector<std::string> source_lines;
    bool in_code = false;
    std::istringstream input(read_file(source));
    for (std::string raw; std::getline(input, raw);) {
        std::string line = strip_whitespace(raw);
        if (line.starts_with("```")) {
            in_code = !in_code;
            continue;
        }
        if (in_code || line.size() < 40 || starts_with_any(line, {"#", "|", "![", "<"})) continue;
        line = std::regex_replace(std::regex_replace(line, bullet, ""), markup, "");
        if (auto normalized = lowercase_words(line); !normalized.empty()) source_lines.push_back(normalized);
    }
    std::string chapter_text;
    for (const auto& path : sorted_files(workspace.build / "EPUB" / "text", ".xhtml", false)) {
        xmlDocPtr doc = xmlReadFile(path.c_str(), nullptr, XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
        if (!doc) continue;
        if (!chapter_text.empty()) chapter_text += ' ';
        chapter_text += lowercase_words(html::text(reinterpret_cast<xmlNodePtr>(doc)));
        xmlFreeDoc(doc);
    }
    json missing = json::array();
    for (const auto& line : source_lines) {
        if (chapter_text.find(line) == std::string::npos) missing.push_back(line);
    }
    double ratio = source_lines.empty() ? 1.0 : 1.0 - static_cast<double>(missing.size()) / source_lines.size();
    workspace.write_json(workspace.checks / "coverage.json",
                         {{"checked", source_lines.size()}, {"missing", missing}, {"coverage", ratio}});
    if (ratio < 0.9) {
        findings.push_back(error_finding("COVERAGE_TEXT_BELOW_THRESHOLD",
                                         std::format("Text coverage is {:.1f}%; expected at least 90%.", ratio * 100),
                                         "validate", source.string()));
    }
    return findings;
}

}  // namespace

StageResult validate(const fs::path& epub_path, const std::optional<fs::path>& workspace_path, bool run_reflow) {
    auto epub = fs::weakly_canonical(fs::absolute(epub_path));
    auto checks = workspace_path ? Workspace(*workspace_path).checks
                                 : epub.parent_path() / (epub.stem().string() + "-checks");
    fs::create_directories(checks);
    auto extracted = checks / "extracted";
    auto findings = extract(epub, extracted);
    auto append = [&](std::vector<Finding> more) { findings.insert(findings.end(), more.begin(), more.end()); };
    append(content_findings(extracted));
    append(epubcheck(epub, checks));
    if (workspace_path) append(coverage(Workspace(*workspace_path)));
    if (run_reflow) append(reflow(extracted, checks));
    int errors = static_cast<int>(std::count_if(findings.begin(), findings.end(),
                                                [](const auto& f) { return f.severity == "error"; }));
    int warnings = static_cast<int>(std::count_if(findings.begin(), findings.end(),
                                                  [](const auto& f) { return f.severity == "warning"; }));
    StageResult result{"validate", errors ? "failed" : "passed", {{"errors", errors}, {"warnings", warnings}}, findings};
    write_file(checks / "validation.json", json(result).dump(2) + "\n");
    return result;
}

}  // namespace docs2epub
