#pragma once

#include "model.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace docs2epub {

struct BuiltBook {
    std::filesystem::path epub;
    int chapters = 0;
    int assets = 0;
};

std::string clean_fragment(const PageRecord& page, const std::map<std::string, std::string>& url_to_file,
                           const std::map<std::string, std::string>& asset_names);

BuiltBook build(const std::filesystem::path& workspace, const std::optional<std::filesystem::path>& output);

}  // namespace docs2epub
