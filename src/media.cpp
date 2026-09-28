#include "media.hpp"

#include "core.hpp"

#include <curl/curl.h>
#include <vips/vips8>

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <stdexcept>

namespace webspine {
namespace {

using vips::VImage;

void ensure_curl() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

void ensure_vips() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (VIPS_INIT("webspine")) throw std::runtime_error("Could not initialize libvips");
    });
}

size_t append_body(char* data, size_t size, size_t count, void* target) {
    static_cast<std::string*>(target)->append(data, size * count);
    return size * count;
}

constexpr int unbounded = 10'000'000;

}  // namespace

HttpResponse http_get(const std::string& url, long timeout_seconds) {
    ensure_curl();
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("Could not initialize libcurl");
    HttpResponse response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "webspine/0.1");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    CURLcode code = curl_easy_perform(curl);
    if (code == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
        char* type = nullptr;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &type);
        if (type) response.content_type = type;
    }
    curl_easy_cleanup(curl);
    if (code != CURLE_OK) throw std::runtime_error(curl_easy_strerror(code));
    return response;
}

std::string extension_for_media_type(std::string_view media_type) {
    static const std::map<std::string, std::string> extensions = {
        {"image/avif", ".avif"},  {"image/bmp", ".bmp"},
        {"image/gif", ".gif"},    {"image/jpeg", ".jpg"},
        {"image/png", ".png"},    {"image/svg+xml", ".svg"},
        {"image/tiff", ".tiff"},  {"image/webp", ".webp"},
        {"image/x-icon", ".ico"}, {"image/vnd.microsoft.icon", ".ico"}};
    std::string type = lower(strip_whitespace(media_type.substr(0, media_type.find(';'))));
    auto it = extensions.find(type);
    return it == extensions.end() ? "" : it->second;
}

std::string media_type_for_file(const std::filesystem::path& path) {
    static const std::map<std::string, std::string> types = {
        {".avif", "image/avif"},   {".css", "text/css"},    {".gif", "image/gif"},
        {".jpeg", "image/jpeg"},   {".jpg", "image/jpeg"},  {".png", "image/png"},
        {".svg", "image/svg+xml"}, {".webp", "image/webp"}, {".xhtml", "application/xhtml+xml"}};
    auto it = types.find(lower(path.extension().string()));
    return it == types.end() ? "application/octet-stream" : it->second;
}

void rasterize_svg(std::string_view svg, const std::filesystem::path& png) {
    ensure_vips();
    VImage image = VImage::thumbnail_buffer(const_cast<char*>(svg.data()), svg.size(), 1600,
                                            VImage::option()->set("height", unbounded));
    image.pngsave(png.c_str());
}

std::string normalize_image(const std::filesystem::path& source, const std::filesystem::path& images_dir) {
    ensure_vips();
    std::string stem = source.stem().string();
    if (lower(source.extension().string()) == ".svg") {
        VImage image = VImage::thumbnail(source.c_str(), 1600, VImage::option()->set("height", unbounded));
        image.pngsave((images_dir / (stem + ".png")).c_str());
        return stem + ".png";
    }
    const char* loader = vips_foreign_find_load(source.c_str());
    if (!loader) throw std::runtime_error("Unrecognized image format");
    // Loader class names look like "VipsForeignLoadWebpFile".
    std::string format = lower(std::string(loader).substr(std::string_view("VipsForeignLoad").size()));
    VImage image =
        VImage::thumbnail(source.c_str(), 2000, VImage::option()->set("height", 2400)->set("size", VIPS_SIZE_DOWN));
    bool convert = starts_with_any(format, {"webp", "heif", "tiff", "magick"});
    std::string output_name = convert ? stem + ".png" : source.filename().string();
    image.write_to_file((images_dir / output_name).c_str());
    return output_name;
}

void render_cover(std::string_view title, const std::filesystem::path& jpeg) {
    ensure_vips();
    constexpr int width = 1200;
    constexpr int height = 1600;
    VImage canvas = VImage::black(width, height).new_from_image(std::vector<double>{0xf4, 0xf0, 0xe7});
    auto overlay = [&](std::string_view text, const char* font, int top, std::vector<double> color) {
        VImage mask = VImage::text(escape_html(text).c_str(), VImage::option()
                                                                  ->set("font", font)
                                                                  ->set("width", 980)
                                                                  ->set("align", VIPS_ALIGN_CENTRE)
                                                                  ->set("spacing", 24));
        int left = std::max(0, (width - mask.width()) / 2);
        VImage placed = mask.embed(left, top, width, height);
        canvas = placed.ifthenelse(color, canvas, VImage::option()->set("blend", true));
    };
    overlay("DOCUMENTATION", "sans 30", 390, {0x68, 0x5f, 0x52});
    if (!strip_whitespace(title).empty()) overlay(title, "sans 76", 520, {0x24, 0x21, 0x1d});
    canvas.cast(VIPS_FORMAT_UCHAR).jpegsave(jpeg.c_str(), VImage::option()->set("Q", 90)->set("optimize_coding", true));
}

}  // namespace webspine
