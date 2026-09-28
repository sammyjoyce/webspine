#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace webspine {

using json = nlohmann::ordered_json;

inline constexpr int ir_version = 1;

struct NavNode {
    std::string title;
    std::optional<std::string> url;
    std::vector<NavNode> children;
};

struct PageRecord {
    std::string url;
    std::string route;
    std::string title;
    std::string language;
    std::string html;
    std::string content_hash;
    std::vector<std::string> discovered_from;
    std::vector<std::string> assets;
    std::vector<std::string> warnings;
};

struct SiteRecord {
    std::string base_url;
    std::string title;
    std::string language;
    std::string adapter;
    int ir_version = webspine::ir_version;
    std::vector<std::string> sitemap_urls;
    std::vector<NavNode> nav;
    std::vector<std::string> pages;
};

struct Finding {
    std::string code;
    std::string severity;
    std::string message;
    std::string stage;
    std::optional<std::string> url;
    std::optional<std::string> file;
    std::optional<std::string> hint;
};

struct StageResult {
    std::string stage;
    std::string status;
    std::map<std::string, int> counts;
    std::vector<Finding> findings;
};

struct Report {
    std::string command;
    std::string status;
    std::string workspace;
    std::optional<std::string> epub;
    std::vector<StageResult> stages;
    int format_version = 1;
};

Finding error_finding(std::string code, std::string message, std::string stage = "validate",
                      std::optional<std::string> file = std::nullopt);

void to_json(json& j, const NavNode& value);
void from_json(const json& j, NavNode& value);
void to_json(json& j, const PageRecord& value);
void from_json(const json& j, PageRecord& value);
void to_json(json& j, const SiteRecord& value);
void from_json(const json& j, SiteRecord& value);
void to_json(json& j, const Finding& value);
void to_json(json& j, const StageResult& value);
void to_json(json& j, const Report& value);

}  // namespace webspine
