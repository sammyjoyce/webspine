#include "model.hpp"

namespace webspine {
namespace {

json optional_json(const std::optional<std::string>& value) { return value ? json(*value) : json(nullptr); }

std::optional<std::string> optional_string(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return std::nullopt;
    return it->get<std::string>();
}

template <typename T>
std::vector<T> list_or_empty(const json& j, const char* key) {
    auto it = j.find(key);
    return it == j.end() ? std::vector<T>{} : it->get<std::vector<T>>();
}

}  // namespace

Finding error_finding(std::string code, std::string message, std::string stage, std::optional<std::string> file) {
    return {std::move(code), "error",         std::move(message), std::move(stage),
            std::nullopt,    std::move(file), std::nullopt};
}

void to_json(json& j, const NavNode& value) {
    j = {{"title", value.title}, {"url", optional_json(value.url)}, {"children", value.children}};
}

void from_json(const json& j, NavNode& value) {
    value.title = j.at("title").get<std::string>();
    value.url = optional_string(j, "url");
    value.children = list_or_empty<NavNode>(j, "children");
}

void to_json(json& j, const PageRecord& value) {
    j = {{"url", value.url},
         {"route", value.route},
         {"title", value.title},
         {"language", value.language},
         {"html", value.html},
         {"content_hash", value.content_hash},
         {"discovered_from", value.discovered_from},
         {"assets", value.assets},
         {"warnings", value.warnings}};
}

void from_json(const json& j, PageRecord& value) {
    value.url = j.at("url").get<std::string>();
    value.route = j.at("route").get<std::string>();
    value.title = j.at("title").get<std::string>();
    value.language = j.at("language").get<std::string>();
    value.html = j.at("html").get<std::string>();
    value.content_hash = j.at("content_hash").get<std::string>();
    value.discovered_from = j.at("discovered_from").get<std::vector<std::string>>();
    value.assets = list_or_empty<std::string>(j, "assets");
    value.warnings = list_or_empty<std::string>(j, "warnings");
}

void to_json(json& j, const SiteMetadata& value) {
    j = {{"name", value.name},
         {"description", value.description},
         {"author", value.author},
         {"captured_on", value.captured_on},
         {"theme_colors", value.theme_colors},
         {"logo", optional_json(value.logo)},
         {"icon", optional_json(value.icon)}};
}

void from_json(const json& j, SiteMetadata& value) {
    value.name = j.value("name", "");
    value.description = j.value("description", "");
    value.author = j.value("author", "");
    value.captured_on = j.value("captured_on", "");
    value.theme_colors = list_or_empty<std::string>(j, "theme_colors");
    value.logo = optional_string(j, "logo");
    value.icon = optional_string(j, "icon");
}

void to_json(json& j, const SiteRecord& value) {
    j = {{"base_url", value.base_url}, {"title", value.title},           {"language", value.language},
         {"adapter", value.adapter},   {"ir_version", value.ir_version}, {"sitemap_urls", value.sitemap_urls},
         {"nav", value.nav},           {"pages", value.pages},           {"metadata", value.metadata}};
}

void from_json(const json& j, SiteRecord& value) {
    value.base_url = j.at("base_url").get<std::string>();
    value.title = j.at("title").get<std::string>();
    value.language = j.at("language").get<std::string>();
    value.adapter = j.at("adapter").get<std::string>();
    value.ir_version = j.at("ir_version").get<int>();
    value.sitemap_urls = j.at("sitemap_urls").get<std::vector<std::string>>();
    value.nav = j.at("nav").get<std::vector<NavNode>>();
    value.pages = j.at("pages").get<std::vector<std::string>>();
    if (auto it = j.find("metadata"); it != j.end()) value.metadata = it->get<SiteMetadata>();
}

void to_json(json& j, const Finding& value) {
    j = {{"code", value.code},
         {"severity", value.severity},
         {"message", value.message},
         {"stage", value.stage},
         {"url", optional_json(value.url)},
         {"file", optional_json(value.file)},
         {"hint", optional_json(value.hint)}};
}

void to_json(json& j, const StageResult& value) {
    json counts = json::object();
    for (const auto& [key, count] : value.counts) counts[key] = count;
    j = {{"stage", value.stage}, {"status", value.status}, {"counts", counts}, {"findings", value.findings}};
}

void to_json(json& j, const Report& value) {
    j = {{"command", value.command},          {"status", value.status}, {"workspace", value.workspace},
         {"epub", optional_json(value.epub)}, {"stages", value.stages}, {"format_version", value.format_version}};
}

}  // namespace webspine
