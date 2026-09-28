#include "workspace.hpp"

#include "core.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace webspine {

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read " + path.string());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void write_file(const fs::path& path, std::string_view data) {
    if (path.has_parent_path()) fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Cannot write " + path.string());
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
}

Workspace::Workspace(const fs::path& path)
    : root(fs::weakly_canonical(fs::absolute(path))),
      pages(root / "pages"),
      assets(root / "assets"),
      build(root / "build"),
      dist(root / "dist"),
      checks(root / "checks"),
      brand(root / "brand") {}

void Workspace::create() const {
    for (const auto& path : {pages, assets, build, dist, checks, brand}) fs::create_directories(path);
}

void Workspace::write_json(const fs::path& path, const json& value) const {
    auto temporary = path;
    temporary += ".tmp";
    write_file(temporary, value.dump(2, ' ', false, json::error_handler_t::replace) + "\n");
    fs::rename(temporary, path);
}

void Workspace::write_site(const SiteRecord& site) const { write_json(root / "site.json", site); }

SiteRecord Workspace::read_site() const {
    auto value = json::parse(read_file(root / "site.json"));
    int version = value.at("ir_version").get<int>();
    if (version != ir_version) {
        throw std::runtime_error("Workspace IR version " + std::to_string(version) + " is not supported; expected " +
                                 std::to_string(ir_version) + ".");
    }
    return value.get<SiteRecord>();
}

void Workspace::write_page(const PageRecord& page) const { write_json(pages / (page_key(page.url) + ".json"), page); }

std::vector<PageRecord> Workspace::read_pages() const {
    std::vector<fs::path> paths;
    if (fs::exists(pages)) {
        for (const auto& entry : fs::directory_iterator(pages)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") paths.push_back(entry.path());
        }
    }
    std::sort(paths.begin(), paths.end());
    std::vector<PageRecord> records;
    for (const auto& path : paths) records.push_back(json::parse(read_file(path)).get<PageRecord>());
    return records;
}

void Workspace::write_report(const json& value) const { write_json(root / "report.json", value); }

}  // namespace webspine
