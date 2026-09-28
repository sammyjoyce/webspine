#include "scrape.hpp"

#include "browser.hpp"
#include "core.hpp"
#include "html.hpp"
#include "media.hpp"
#include "workspace.hpp"

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace webspine {
namespace {

const std::vector<std::string> content_selectors = {"main article", "article", "main [class*='content']", "main",
                                                    "[role='main']"};

const std::vector<std::string> nav_selectors = {"#sidebar-content", "nav[aria-label*='doc' i]", "aside nav", "aside"};

constexpr std::string_view extract_js = R"JS(
({contentSelectors, navSelectors}) => {
  document.querySelectorAll('details').forEach((node) => node.open = true);
  document.querySelectorAll('[aria-expanded="false"]').forEach((node) => {
    try { node.click(); } catch (_) {}
  });
  document.querySelectorAll('[role="tab"]').forEach((tab) => {
    const panelId = tab.getAttribute('aria-controls');
    const panel = panelId ? document.getElementById(panelId) : null;
    if (panel) {
      panel.hidden = false;
      panel.removeAttribute('aria-hidden');
      const label = document.createElement('h3');
      label.textContent = tab.textContent.trim();
      panel.prepend(label);
    }
  });
  const content = contentSelectors.map((s) => document.querySelector(s)).find(Boolean);
  if (!content) return {error: 'content-not-found'};
  content.querySelectorAll('img').forEach((image) => {
    if (image.currentSrc) image.src = image.currentSrc;
    image.removeAttribute('srcset');
  });
  content.querySelectorAll('*').forEach((node) => {
    if (getComputedStyle(node).display === 'none') node.remove();
  });
  const clone = content.cloneNode(true);
  clone.querySelectorAll('script,style,noscript,button,input,textarea,select,nav,[role="navigation"],[aria-hidden="true"]').forEach((node) => node.remove());
  clone.querySelectorAll('*').forEach((node) => {
    node.removeAttribute('style');
    node.removeAttribute('class');
    node.removeAttribute('onclick');
    if (node.tagName === 'IFRAME') {
      const link = document.createElement('a');
      link.href = node.src;
      link.textContent = node.title || 'Embedded content';
      node.replaceWith(link);
    }
  });
  const nav = navSelectors.map((s) => document.querySelector(s)).find(Boolean);
  const links = nav ? [...nav.querySelectorAll('a[href]')].map((a) => ({
    title: a.textContent.trim(), href: a.href
  })) : [];
  return {
    location: location.href,
    title: document.querySelector('h1')?.textContent.trim() || document.title,
    language: document.documentElement.lang || 'en',
    html: clone.innerHTML,
    nav: links,
  };
}
)JS";

template <typename T>
void append_unique(std::vector<T>& values, std::set<T>& seen, const T& value) {
    if (seen.insert(value).second) values.push_back(value);
}

std::vector<std::string> candidate_urls(const std::string& base_url, const std::string& file) {
    Url base = parse_url(base_url);
    std::string trimmed = base_url;
    while (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();
    std::vector<std::string> out = {trimmed + "/" + file};
    std::string root = base.scheme + "://" + base.netloc + "/" + file;
    if (root != out.front()) out.push_back(root);
    return out;
}

std::vector<std::string> sitemap_locations(const std::string& body) {
    std::vector<std::string> out;
    xmlDocPtr doc = xmlReadMemory(body.data(), static_cast<int>(body.size()), nullptr, nullptr,
                                  XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) return out;
    for (auto node :
         html::elements(reinterpret_cast<xmlNodePtr>(doc), [](xmlNodePtr node) { return html::name(node) == "loc"; })) {
        out.push_back(strip_whitespace(html::text(node)));
    }
    xmlFreeDoc(doc);
    return out;
}

std::vector<std::string> sitemap_urls(const std::string& base_url) {
    for (const auto& candidate : candidate_urls(base_url, "sitemap.xml")) {
        try {
            auto response = http_get(candidate, 20);
            if (!response.ok()) continue;
            std::vector<std::string> urls;
            std::set<std::string> seen;
            for (const auto& location : sitemap_locations(response.body)) {
                if (!location.empty() && in_scope(location, base_url)) {
                    append_unique(urls, seen, canonical_url(location));
                }
            }
            if (!urls.empty()) return urls;
        } catch (const std::exception&) {
        }
    }
    return {};
}

void cache_text_source(const std::string& base_url, const Workspace& workspace) {
    for (const auto& candidate : candidate_urls(base_url, "llms-full.txt")) {
        try {
            auto response = http_get(candidate, 20);
            if (response.ok() && !strip_whitespace(response.body).empty()) {
                write_file(workspace.root / "source" / "llms-full.txt", response.body);
                return;
            }
        } catch (const std::exception&) {
        }
    }
}

std::string asset_name(const std::string& url, const std::string& media_type) {
    std::string extension = extension_for_media_type(media_type);
    if (extension.empty()) extension = fs::path(parse_url(url).path).extension().string();
    if (extension.empty()) extension = ".bin";
    return sha256_hex(url).substr(0, 20) + extension;
}

struct Downloaded {
    std::string html;
    std::vector<std::string> assets;
    std::vector<std::string> warnings;
};

Downloaded download_assets(const std::string& page_url, const std::string& markup, const Workspace& workspace) {
    html::Fragment fragment(markup);
    Downloaded out;
    std::set<std::string> rasterized;
    for (auto diagram : html::elements(fragment.root(), [](auto node) { return html::name(node) == "svg"; })) {
        if (html::ancestor(diagram, "svg")) continue;
        try {
            std::string data = html::outer_xml(diagram);
            if (data.find("xmlns=") == std::string::npos) {
                data.insert(4, " xmlns=\"http://www.w3.org/2000/svg\"");
            }
            std::string name = sha256_hex(data).substr(0, 20) + ".png";
            auto path = workspace.assets / name;
            if (!fs::exists(path)) rasterize_svg(data, path);
            auto image = fragment.create("img");
            html::set_attr(image, "src", "assets/" + name);
            html::set_attr(
                image, "alt",
                html::attr(diagram, "aria-label").value_or(html::attr(diagram, "title").value_or("Diagram")));
            fragment.replace(diagram, image);
            out.assets.push_back(name);
            rasterized.insert(name);
        } catch (const std::exception& error) {
            out.warnings.push_back(std::string("Could not rasterize an inline SVG: ") + error.what());
        }
    }
    for (auto image : html::elements(fragment.root(), [](auto node) { return html::name(node) == "img"; })) {
        auto source = html::attr(image, "src");
        if (!source || source->empty() || source->starts_with("data:") ||
            rasterized.count(fs::path(*source).filename().string())) {
            continue;
        }
        std::string url = url_join(page_url, *source);
        try {
            auto response = http_get(url, 30);
            if (!response.ok()) throw std::runtime_error("HTTP " + std::to_string(response.status));
            std::string name = asset_name(url, response.content_type);
            auto path = workspace.assets / name;
            if (!fs::exists(path)) write_file(path, response.body);
            html::set_attr(image, "src", "assets/" + name);
            html::remove_attr(image, "srcset");
            out.assets.push_back(name);
        } catch (const std::exception& error) {
            out.warnings.push_back("Could not download image " + url + ": " + error.what());
        }
    }
    out.html = fragment.xml();
    return out;
}

json render(Browser& browser, const std::string& url, const std::vector<std::string>& content,
            const std::vector<std::string>& nav) {
    browser.navigate(url, true);
    browser.emulate_light_reduced_motion();
    browser.evaluate("window.scrollTo(0, document.body.scrollHeight)");
    browser.sleep_ms(100);
    json arguments = {{"contentSelectors", content}, {"navSelectors", nav}};
    return browser.evaluate("(" + std::string(extract_js) + ")(" + arguments.dump() + ")");
}

std::string string_field(const json& value, const char* key) {
    auto it = value.find(key);
    return it != value.end() && it->is_string() ? it->get<std::string>() : std::string();
}

}  // namespace

SiteRecord scrape(const ScrapeOptions& options) {
    std::string base_url = canonical_url(options.url);
    Workspace workspace(options.workspace);
    workspace.create();
    cache_text_source(base_url, workspace);
    std::vector<std::string> content = content_selectors;
    if (options.content_selector) content.insert(content.begin(), *options.content_selector);
    std::vector<std::string> nav_order = nav_selectors;
    if (options.nav_selector) nav_order.insert(nav_order.begin(), *options.nav_selector);
    auto sitemap = sitemap_urls(base_url);
    std::set<std::string> sitemap_set(sitemap.begin(), sitemap.end());
    std::set<std::string> cached;
    for (const auto& page : workspace.read_pages()) cached.insert(page.url);

    Browser browser;
    browser.set_viewport(1280, 900);
    json first = render(browser, base_url, content, nav_order);
    if (first.contains("error")) {
        throw std::runtime_error("No documentation content root was found. Pass --content-selector for this site.");
    }

    std::vector<std::pair<std::string, std::string>> nav_links;
    std::set<std::string> nav_urls;
    for (const auto& link : first.value("nav", json::array())) {
        std::string title = strip_whitespace(string_field(link, "title"));
        std::string href = string_field(link, "href");
        if (title.empty() || !in_scope(href, base_url)) continue;
        std::string url = canonical_url(href);
        if (nav_urls.insert(url).second) nav_links.emplace_back(title, url);
    }
    std::vector<std::string> discovered;
    std::set<std::string> discovered_set;
    append_unique(discovered, discovered_set, base_url);
    for (const auto& [title, url] : nav_links) append_unique(discovered, discovered_set, url);
    for (const auto& url : sitemap) append_unique(discovered, discovered_set, url);
    if (static_cast<int>(discovered.size()) > options.max_pages) {
        throw std::runtime_error("Discovered " + std::to_string(discovered.size()) + " pages, above --max-pages " +
                                 std::to_string(options.max_pages) + ".");
    }

    // A discovered URL can redirect to another page. Pages are stored under the URL they
    // land on, so an alias and its target become one chapter.
    std::map<std::string, std::string> landed_on;
    std::set<std::string> captured = options.force ? std::set<std::string>{} : cached;
    for (const auto& discovered_url : discovered) {
        if (captured.count(discovered_url)) continue;
        json rendered = discovered_url == base_url ? first : render(browser, discovered_url, content, nav_order);
        if (rendered.contains("error")) continue;
        std::string url = canonical_url(string_field(rendered, "location"));
        if (url.empty() || !in_scope(url, base_url)) continue;
        landed_on[discovered_url] = url;
        if (!captured.insert(url).second) continue;
        auto downloaded = download_assets(url, string_field(rendered, "html"), workspace);
        std::vector<std::string> sources;
        if (sitemap_set.count(url)) sources.push_back("sitemap");
        if (url == base_url || nav_urls.count(url)) sources.push_back("navigation");
        if (sources.empty()) sources.push_back("entrypoint");
        std::string route = parse_url(url).path;
        PageRecord record{url,
                          route.empty() ? "/" : route,
                          strip_whitespace(string_field(rendered, "title")),
                          options.language.value_or(string_field(rendered, "language")),
                          downloaded.html,
                          sha256_hex(downloaded.html),
                          sources,
                          downloaded.assets,
                          downloaded.warnings};
        workspace.write_page(record);
    }

    auto resolve = [&](const std::string& url) {
        auto it = landed_on.find(url);
        return it == landed_on.end() ? url : it->second;
    };
    auto pages = workspace.read_pages();
    std::map<std::string, std::string> titles;
    for (const auto& page : pages) titles[page.url] = page.title;
    std::vector<NavNode> nav;
    std::set<std::string> in_nav;
    for (const auto& [title, alias] : nav_links) {
        auto url = resolve(alias);
        if (titles.count(url) && in_nav.insert(url).second) nav.push_back({title, url, {}});
    }
    for (const auto& alias : discovered) {
        auto url = resolve(alias);
        if (titles.count(url) && in_nav.insert(url).second) nav.push_back({titles[url], url, {}});
    }
    std::vector<std::string> resolved_sitemap;
    std::set<std::string> seen_sitemap;
    for (const auto& url : sitemap) append_unique(resolved_sitemap, seen_sitemap, resolve(url));
    sitemap = resolved_sitemap;
    std::vector<std::string> page_urls;
    for (const auto& [url, title] : titles) page_urls.push_back(url);
    SiteRecord site{base_url,
                    options.title.value_or(string_field(first, "title")),
                    options.language.value_or(string_field(first, "language")),
                    "generic",
                    ir_version,
                    sitemap,
                    nav,
                    page_urls};
    workspace.write_site(site);
    return site;
}

}  // namespace webspine
