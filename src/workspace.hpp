#pragma once

#include "model.hpp"

#include <filesystem>
#include <string_view>
#include <vector>

namespace webspine {

namespace fs = std::filesystem;

std::string read_file(const fs::path& path);
void write_file(const fs::path& path, std::string_view data);

class Workspace {
  public:
    explicit Workspace(const fs::path& root);

    fs::path root, pages, assets, build, dist, checks;

    void create() const;
    void write_json(const fs::path& path, const json& value) const;
    void write_site(const SiteRecord& site) const;
    SiteRecord read_site() const;
    void write_page(const PageRecord& page) const;
    std::vector<PageRecord> read_pages() const;
    void write_report(const json& value) const;
};

}  // namespace webspine
