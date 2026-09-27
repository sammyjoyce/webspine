#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace docs2epub {

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
void render_cover(std::string_view title, const std::filesystem::path& jpeg);

}  // namespace docs2epub
