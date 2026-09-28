#pragma once

#include "model.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace webspine {

struct BuiltBook {
    std::filesystem::path epub;
    int chapters = 0;
    int assets = 0;
};

std::string clean_fragment(const PageRecord& page, const std::map<std::string, std::string>& url_to_file,
                           const std::map<std::string, std::string>& asset_names);

// Removes #fragments that name no ID in their target chapter (EPUB 3.4 section 4.2.5).
// A link whose only target was the missing fragment in its own chapter becomes plain text.
std::string prune_dangling_fragments(const std::string& content, const std::string& own_file,
                                     const std::map<std::string, std::set<std::string>>& ids_by_file);

BuiltBook build(const std::filesystem::path& workspace, const std::optional<std::filesystem::path>& output);

}  // namespace webspine
