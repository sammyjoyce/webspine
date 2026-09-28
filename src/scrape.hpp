#pragma once

#include "model.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace webspine {

struct ScrapeOptions {
    std::string url;
    std::filesystem::path workspace;
    std::optional<std::string> title;
    std::optional<std::string> language;
    std::optional<std::string> content_selector;
    std::optional<std::string> nav_selector;
    int max_pages = 500;
    bool force = false;
};

SiteRecord scrape(const ScrapeOptions& options);

}  // namespace webspine
