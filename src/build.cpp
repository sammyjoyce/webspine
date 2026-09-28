#include "build.hpp"

#include "book_css.hpp"
#include "core.hpp"
#include "html.hpp"
#include "media.hpp"
#include "workspace.hpp"

#include <zip.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <regex>
#include <set>
#include <stdexcept>

namespace webspine {
namespace {

const std::set<std::string> allowed_tags = {
    "a",      "abbr",    "aside",  "b",      "bdi",   "bdo",        "blockquote", "br",   "caption", "cite",
    "code",   "dd",      "del",    "details", "dfn",  "div",        "dl",         "dt",   "em",      "figcaption",
    "figure", "h1",      "h2",     "h3",     "h4",    "h5",         "h6",         "hr",   "i",       "img",
    "kbd",    "li",      "mark",   "ol",     "p",     "pre",        "q",          "rp",   "rt",      "ruby",
    "s",      "samp",    "section", "small", "span",  "strong",     "sub",        "summary", "sup",  "table",
    "tbody",  "td",      "tfoot",  "th",     "thead", "time",       "tr",         "u",    "ul",      "var",
    "wbr",
    // Presentation MathML (EPUB 3.4 section 7.1.4.2).
    "math",   "mi",      "mn",     "mo",     "ms",    "mtext",      "mspace",     "mrow", "mfrac",   "msqrt",
    "mroot",  "mstyle",  "msub",   "msup",   "msubsup", "munder",   "mover",      "munderover", "mtable", "mtr",
    "mtd",    "semantics", "annotation"};

// abbr is absent although HTML allows it on th: EPUBCheck 5.3.0 rejects it.
const std::set<std::string> allowed_attributes = {
    "id",    "href",  "src",      "alt",      "title", "colspan", "rowspan", "scope",   "lang", "dir",
    "start", "reversed", "value", "datetime", "cite",  "width",   "height",  "headers", "xmlns", "display",
    "alttext", "mathvariant", "encoding"};

const std::set<std::string> block_containers = {"aside", "blockquote", "body", "dd", "div", "figure",
                                                "li",    "section",    "td",   "th"};

void convert_tabular_pre(html::Fragment& fragment) {
    static const std::regex column_gap(R"(\s{2,})");
    for (auto pre : html::elements(fragment.root(), [](auto node) { return html::name(node) == "pre"; })) {
        std::vector<std::vector<std::string>> rows;
        std::string content = html::text(pre);
        size_t start = 0;
        while (start <= content.size()) {
            auto end = content.find_first_of("\n\r", start);
            auto line = strip_whitespace(content.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (!line.empty()) {
                rows.emplace_back(std::sregex_token_iterator(line.begin(), line.end(), column_gap, -1),
                                  std::sregex_token_iterator());
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (rows.size() < 3) continue;
        std::map<size_t, int> frequency;
        for (const auto& row : rows) ++frequency[row.size()];
        size_t dominant = 0;
        int dominant_count = 0;
        for (const auto& [columns, count] : frequency) {
            if (count > dominant_count) {
                dominant = columns;
                dominant_count = count;
            }
        }
        if (dominant < 3 || static_cast<double>(dominant_count) / rows.size() < 0.75) continue;
        auto table = fragment.create("table");
        auto body = fragment.create("tbody");
        xmlAddChild(table, body);
        for (size_t row_index = 0; row_index < rows.size(); ++row_index) {
            const auto& values = rows[row_index];
            auto row = fragment.create("tr");
            xmlAddChild(body, row);
            if (values.size() != dominant) {
                std::string joined;
                for (const auto& value : values) joined += (joined.empty() ? "" : "  ") + value;
                auto cell = fragment.create("td", joined);
                html::set_attr(cell, "colspan", std::to_string(dominant));
                xmlAddChild(row, cell);
                continue;
            }
            for (const auto& value : values) {
                auto cell = fragment.create(row_index == 0 ? "th" : "td", value);
                if (row_index == 0) html::set_attr(cell, "scope", "col");
                xmlAddChild(row, cell);
            }
        }
        fragment.replace(pre, table);
    }
}

void normalize_headings(html::Fragment& fragment) {
    int previous = 1;
    for (auto heading : html::elements(fragment.root(), html::is_heading)) {
        int level = std::min(std::max(html::heading_level(heading), 2), previous + 1);
        html::rename(heading, "h" + std::to_string(level));
        previous = level;
    }
}

std::string timestamp() {
    auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", now);
}

std::string chapter_xhtml(const PageRecord& page, const std::string& content, const std::string& language) {
    auto lang = escape_html(language);
    auto title = escape_html(page.title);
    auto url = escape_html(page.url);
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!DOCTYPE html>\n"
           "<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"" + lang + "\" lang=\"" + lang + "\">\n"
           "<head>\n  <meta charset=\"utf-8\"/>\n"
           "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"/>\n"
           "  <title>" + title + "</title>\n"
           "  <link rel=\"stylesheet\" type=\"text/css\" href=\"../styles/book.css\"/>\n</head>\n"
           "<body>\n  <main>\n    <h1>" + title + "</h1>\n"
           "    <p class=\"source\">Source: <a href=\"" + url + "\">" + url + "</a></p>\n    " + content +
           "\n  </main>\n</body>\n</html>\n";
}

std::map<std::string, std::string> normalize_assets(const Workspace& workspace, const fs::path& images_dir) {
    std::vector<fs::path> sources;
    if (fs::exists(workspace.assets)) {
        for (const auto& entry : fs::directory_iterator(workspace.assets)) {
            if (entry.is_regular_file()) sources.push_back(entry.path());
        }
    }
    std::sort(sources.begin(), sources.end());
    std::map<std::string, std::string> result;
    for (const auto& source : sources) {
        std::string output_name = source.filename().string();
        try {
            output_name = normalize_image(source, images_dir);
        } catch (const std::exception&) {
            fs::copy_file(source, images_dir / output_name, fs::copy_options::overwrite_existing);
        }
        result[source.filename().string()] = output_name;
    }
    return result;
}

void write_zip(const fs::path& build_root, const fs::path& output) {
    auto temporary = output;
    temporary += ".tmp";
    fs::remove(temporary);
    int error = 0;
    zip_t* archive = zip_open(temporary.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
    if (!archive) throw std::runtime_error("Could not create " + temporary.string());
    auto add = [&](const fs::path& path, const std::string& name, bool stored) {
        zip_source_t* source = zip_source_file(archive, path.c_str(), 0, ZIP_LENGTH_TO_END);
        if (!source) throw std::runtime_error(zip_strerror(archive));
        zip_int64_t index = zip_file_add(archive, name.c_str(), source, ZIP_FL_ENC_UTF_8);
        if (index < 0) {
            zip_source_free(source);
            throw std::runtime_error(zip_strerror(archive));
        }
        zip_set_file_compression(archive, static_cast<zip_uint64_t>(index), stored ? ZIP_CM_STORE : ZIP_CM_DEFLATE, 0);
    };
    add(build_root / "mimetype", "mimetype", true);
    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(build_root)) {
        if (entry.is_regular_file() && entry.path().filename() != "mimetype") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& path : files) add(path, fs::relative(path, build_root).generic_string(), false);
    if (zip_close(archive) != 0) {
        std::string message = zip_strerror(archive);
        zip_discard(archive);
        throw std::runtime_error("Could not write EPUB: " + message);
    }
    fs::rename(temporary, output);
}

BuiltBook write_package(const Workspace& workspace, const fs::path& requested_output) {
    SiteRecord site = workspace.read_site();
    auto records = workspace.read_pages();
    std::map<std::string, const PageRecord*> pages_by_url;
    for (const auto& page : records) pages_by_url[page.url] = &page;
    std::vector<const PageRecord*> ordered;
    std::set<std::string> seen;
    for (const auto& node : site.nav) {
        if (node.url && pages_by_url.count(*node.url) && seen.insert(*node.url).second) {
            ordered.push_back(pages_by_url[*node.url]);
        }
    }
    for (const auto& page : records) {
        if (seen.insert(page.url).second) ordered.push_back(&page);
    }
    if (ordered.empty()) throw std::runtime_error("The workspace has no captured pages.");

    fs::remove_all(workspace.build);
    auto epub_root = workspace.build / "EPUB";
    auto text_dir = epub_root / "text";
    auto images_dir = epub_root / "images";
    for (const auto& path : {text_dir, epub_root / "styles", images_dir, workspace.build / "META-INF"}) {
        fs::create_directories(path);
    }
    write_file(workspace.build / "mimetype", "application/epub+zip");
    write_file(workspace.build / "META-INF" / "container.xml",
               "<?xml version=\"1.0\"?>\n"
               "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
               "  <rootfiles><rootfile full-path=\"EPUB/package.opf\" "
               "media-type=\"application/oebps-package+xml\"/></rootfiles>\n</container>\n");
    write_file(epub_root / "styles" / "book.css", book_css);
    render_cover(site.title, images_dir / "cover.jpg");
    auto asset_names = normalize_assets(workspace, images_dir);

    std::map<std::string, std::string> url_to_file;
    for (const auto* page : ordered) url_to_file[page->url] = chapter_name(page->route, page->url);
    struct Chapter {
        std::string id, file, title;
        const PageRecord* page;
        std::string content;
        bool mathml = false;
    };
    std::vector<Chapter> chapters;
    std::map<std::string, std::set<std::string>> ids_by_file;
    for (size_t index = 0; index < ordered.size(); ++index) {
        const auto& page = *ordered[index];
        const auto& file = url_to_file[page.url];
        auto content = clean_fragment(page, url_to_file, asset_names);
        html::Fragment parsed(content);
        auto& ids = ids_by_file[file];
        for (auto node : html::elements(parsed.root())) {
            if (auto id = html::attr(node, "id")) ids.insert(*id);
        }
        bool mathml = html::first_element(parsed.root(), [](auto node) { return html::name(node) == "math"; });
        chapters.push_back({"chapter-" + std::to_string(index + 1), file, page.title, &page, content, mathml});
    }
    for (const auto& chapter : chapters) {
        const auto& page = *chapter.page;
        auto content = prune_dangling_fragments(chapter.content, chapter.file, ids_by_file);
        write_file(text_dir / chapter.file,
                   chapter_xhtml(page, content, page.language.empty() ? site.language : page.language));
    }

    auto lang = escape_html(site.language);
    auto title = escape_html(site.title);
    auto html_open = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!DOCTYPE html>\n"
                     "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\" xml:lang=\"" +
                     lang + "\" lang=\"" + lang + "\">\n";
    write_file(epub_root / "cover.xhtml",
               html_open + "<head><title>" + title +
                   "</title><link rel=\"stylesheet\" type=\"text/css\" href=\"styles/book.css\"/></head>\n"
                   "<body><section class=\"cover\" epub:type=\"cover\"><img src=\"images/cover.jpg\" alt=\"Cover for " +
                   title + "\"/></section></body>\n</html>\n");

    std::string nav_items;
    std::string ncx_points;
    for (size_t index = 0; index < chapters.size(); ++index) {
        auto file = escape_html(chapters[index].file);
        auto label = escape_html(chapters[index].title);
        auto number = std::to_string(index + 1);
        nav_items += (index ? "\n" : "") + std::string("<li><a href=\"text/") + file + "\">" + label + "</a></li>";
        ncx_points += (index ? "\n" : "") + std::string("<navPoint id=\"nav-") + number + "\" playOrder=\"" + number +
                      "\"><navLabel><text>" + label + "</text></navLabel><content src=\"text/" + file +
                      "\"/></navPoint>";
    }
    write_file(epub_root / "nav.xhtml",
               html_open +
                   "<head><title>Contents</title><link rel=\"stylesheet\" type=\"text/css\" href=\"styles/book.css\"/></head>\n"
                   "<body><nav epub:type=\"toc\" id=\"toc\"><h1>Contents</h1><ol>" + nav_items + "</ol></nav>\n"
                   "<nav epub:type=\"landmarks\" hidden=\"hidden\"><ol>\n"
                   "<li><a epub:type=\"cover\" href=\"cover.xhtml\">Cover</a></li>\n"
                   "<li><a epub:type=\"toc\" href=\"nav.xhtml\">Contents</a></li>\n"
                   "<li><a epub:type=\"bodymatter\" href=\"text/" + escape_html(chapters.front().file) +
                   "\">Start</a></li>\n</ol></nav></body>\n</html>\n");

    std::string identifier = "urn:uuid:" + uuid5_url(site.base_url);
    write_file(epub_root / "toc.ncx",
               "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
               "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\" xml:lang=\"" + lang + "\">\n"
               "<head><meta name=\"dtb:uid\" content=\"" + identifier + "\"/></head><docTitle><text>" + title +
                   "</text></docTitle><navMap>" + ncx_points + "</navMap>\n</ncx>\n");

    std::string manifest =
        "<item id=\"cover-page\" href=\"cover.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>"
        "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>"
        "<item id=\"css\" href=\"styles/book.css\" media-type=\"text/css\"/>"
        "<item id=\"cover-image\" href=\"images/cover.jpg\" media-type=\"image/jpeg\" properties=\"cover-image\"/>";
    for (const auto& chapter : chapters) {
        manifest += "<item id=\"" + chapter.id + "\" href=\"text/" + escape_html(chapter.file) +
                    "\" media-type=\"application/xhtml+xml\"" + (chapter.mathml ? " properties=\"mathml\"" : "") + "/>";
    }
    std::vector<fs::path> images;
    for (const auto& entry : fs::directory_iterator(images_dir)) images.push_back(entry.path());
    std::sort(images.begin(), images.end());
    for (size_t index = 0; index < images.size(); ++index) {
        if (images[index].filename() == "cover.jpg") continue;
        manifest += "<item id=\"asset-" + std::to_string(index + 1) + "\" href=\"images/" +
                    escape_html(images[index].filename().string()) + "\" media-type=\"" +
                    media_type_for_file(images[index]) + "\"/>";
    }
    std::string spine = "<itemref idref=\"cover-page\"/>\n<itemref idref=\"nav\" linear=\"no\"/>";
    for (const auto& chapter : chapters) spine += "\n<itemref idref=\"" + chapter.id + "\"/>";
    write_file(epub_root / "package.opf",
               "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
               "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"book-id\" xml:lang=\"" +
                   lang + "\" prefix=\"schema: http://schema.org/\">\n"
                   "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
                   "  <dc:identifier id=\"book-id\">" + identifier + "</dc:identifier>\n"
                   "  <dc:title>" + title + "</dc:title>\n"
                   "  <dc:language>" + lang + "</dc:language>\n"
                   "  <dc:source>" + escape_html(site.base_url) + "</dc:source>\n"
                   "  <dc:publisher>webspine</dc:publisher>\n"
                   "  <meta property=\"dcterms:modified\">" + timestamp() + "</meta>\n"
                   "  <meta property=\"rendition:layout\">reflowable</meta>\n"
                   "  <meta property=\"schema:accessModeSufficient\">textual</meta>\n"
                   "  <meta property=\"schema:accessMode\">textual</meta>\n"
                   "  <meta property=\"schema:accessMode\">visual</meta>\n"
                   "  <meta property=\"schema:accessibilityFeature\">alternativeText</meta>\n"
                   "  <meta property=\"schema:accessibilityFeature\">readingOrder</meta>\n"
                   "  <meta property=\"schema:accessibilityFeature\">tableOfContents</meta>\n"
                   "  <meta property=\"schema:accessibilityHazard\">none</meta>\n"
                   "  <meta property=\"schema:accessibilitySummary\">The publication has a table of contents, source "
                   "links, reflowable text, and alternative text for images.</meta>\n"
                   "</metadata>\n<manifest>" + manifest + "</manifest>\n<spine toc=\"ncx\">" + spine +
                   "</spine>\n</package>\n");

    auto output = fs::weakly_canonical(fs::absolute(requested_output));
    fs::create_directories(output.parent_path());
    write_zip(workspace.build, output);
    return {output, static_cast<int>(chapters.size()), static_cast<int>(asset_names.size())};
}

}  // namespace

std::string clean_fragment(const PageRecord& page, const std::map<std::string, std::string>& url_to_file,
                           const std::map<std::string, std::string>& asset_names) {
    html::Fragment fragment(page.html);
    if (auto first = html::first_element(fragment.root(), html::is_heading);
        first && html::joined_text(first, " ") == page.title) {
        fragment.detach(first);
    }
    convert_tabular_pre(fragment);
    html::remove_comments(fragment.root());
    for (auto tag : html::elements(fragment.root())) {
        if (!allowed_tags.count(html::name(tag))) {
            fragment.unwrap(tag);
            continue;
        }
        for (const auto& key : html::attr_names(tag)) {
            if (!allowed_attributes.count(key)) html::remove_attr(tag, key);
        }
    }
    for (auto details : html::elements(fragment.root(), [](auto node) { return html::name(node) == "details"; })) {
        html::rename(details, "section");
        auto summaries = html::child_elements(details, "summary");
        if (!summaries.empty()) html::rename(summaries.front(), "h3");
    }
    normalize_headings(fragment);
    for (auto math : html::elements(fragment.root(), [](auto node) { return html::name(node) == "math"; })) {
        if (!html::attr(math, "alttext")) html::set_attr(math, "alttext", html::text(math));
    }

    std::set<std::string> used_ids;
    for (auto tag : html::elements(fragment.root())) {
        auto id = html::attr(tag, "id");
        if (!id || id->empty()) continue;
        std::string base = xml_id(*id);
        std::string candidate = base;
        for (int suffix = 2; used_ids.count(candidate); ++suffix) candidate = base + "-" + std::to_string(suffix);
        html::set_attr(tag, "id", candidate);
        used_ids.insert(candidate);
    }

    for (auto link : html::elements(fragment.root(), [](auto node) { return html::name(node) == "a"; })) {
        auto href = html::attr(link, "href");
        if (!href || href->empty()) continue;
        auto scheme = parse_url(*href).scheme;
        if (scheme == "mailto" || scheme == "tel") continue;
        if (!scheme.empty() && scheme != "http" && scheme != "https") {
            // EPUB 3.4 section 3.7 forbids data: hyperlinks; script URLs cannot navigate in a reader.
            fragment.unwrap(link);
            continue;
        }
        auto absolute = url_join(page.url, *href);
        auto [target, anchor] = url_defrag(absolute);
        if (*href == "#") {
            fragment.unwrap(link);
        } else if (href->starts_with("#")) {
            html::set_attr(link, "href", "#" + xml_id(href->substr(1)));
        } else if (auto local = url_to_file.find(canonical_url(target)); local != url_to_file.end()) {
            html::set_attr(link, "href", local->second + (anchor.empty() ? "" : "#" + xml_id(anchor)));
        } else {
            html::set_attr(link, "href", absolute);
        }
    }

    for (auto image : html::elements(fragment.root(), [](auto node) { return html::name(node) == "img"; })) {
        auto name = fs::path(html::attr(image, "src").value_or("")).filename().string();
        if (auto asset = asset_names.find(name); asset != asset_names.end()) {
            html::set_attr(image, "src", "../images/" + asset->second);
        }
        auto alt = html::attr(image, "alt");
        html::set_attr(image, "alt", alt && !alt->empty() ? *alt : "Illustration");
        auto parent = html::name(image->parent);
        if (parent != "figure" && block_containers.count(parent)) fragment.wrap(image, "figure");
    }

    for (auto table : html::elements(fragment.root(), [](auto node) { return html::name(node) == "table"; })) {
        if (html::first_element(table, [](auto node) { return html::name(node) == "th"; })) continue;
        auto first_row = html::first_element(table, [](auto node) { return html::name(node) == "tr"; });
        if (!first_row) continue;
        for (auto cell : html::child_elements(first_row, "td")) {
            html::rename(cell, "th");
            html::set_attr(cell, "scope", "col");
        }
    }
    return fragment.xml();
}

std::string prune_dangling_fragments(const std::string& content, const std::string& own_file,
                                     const std::map<std::string, std::set<std::string>>& ids_by_file) {
    html::Fragment fragment(content);
    for (auto link : html::elements(fragment.root(), [](auto node) { return html::name(node) == "a"; })) {
        auto href = html::attr(link, "href").value_or("");
        auto hash = href.find('#');
        if (hash == std::string::npos || !parse_url(href).scheme.empty()) continue;
        auto file = hash == 0 ? own_file : href.substr(0, hash);
        auto chapter = ids_by_file.find(file);
        if (chapter == ids_by_file.end() || chapter->second.count(href.substr(hash + 1))) continue;
        if (hash == 0) {
            fragment.unwrap(link);
        } else {
            html::set_attr(link, "href", file);
        }
    }
    return fragment.xml();
}

BuiltBook build(const fs::path& workspace_path, const std::optional<fs::path>& output) {
    Workspace workspace(workspace_path);
    SiteRecord site = workspace.read_site();
    return write_package(workspace, output.value_or(workspace.dist / (route_name(site.title) + ".epub")));
}

}  // namespace webspine
