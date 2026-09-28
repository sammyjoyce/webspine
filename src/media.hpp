#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace webspine {

struct HttpResponse {
    long status = 0;
    std::string content_type;
    std::string body;
    bool ok() const { return status >= 200 && status < 300; }
};

// Throws on transport failure; HTTP error statuses are returned, not thrown.
HttpResponse http_get(const std::string& url, long timeout_seconds);

std::string extension_for_media_type(std::string_view media_type);
std::string media_type_for_file(const std::filesystem::path& path);

void rasterize_svg(std::string_view svg, const std::filesystem::path& png);
std::string normalize_image(const std::filesystem::path& source, const std::filesystem::path& images_dir);
struct CoverSpec {
    std::string title;
    std::string description;
    std::string host;
    std::string captured_on;
    std::vector<std::string> theme_colors;
    std::optional<std::filesystem::path> logo;
    std::optional<std::filesystem::path> icon;
};

inline constexpr int cover_width = 1600;
inline constexpr int cover_height = 2560;

void render_cover(const CoverSpec& spec, const std::filesystem::path& jpeg);

}  // namespace webspine
