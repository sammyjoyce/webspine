#include "media.hpp"

#include "core.hpp"

#include <curl/curl.h>
#include <vips/vips8>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
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

namespace {

using Rgb = std::array<double, 3>;

std::optional<Rgb> parse_hex_color(std::string_view value) {
    auto hex = strip_whitespace(value);
    if (hex.starts_with('#')) hex.erase(0, 1);
    if (hex.size() == 3) hex = {hex[0], hex[0], hex[1], hex[1], hex[2], hex[2]};
    if (hex.size() != 6 || hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return std::nullopt;
    return Rgb{double(std::stoi(hex.substr(0, 2), nullptr, 16)), double(std::stoi(hex.substr(2, 2), nullptr, 16)),
               double(std::stoi(hex.substr(4, 2), nullptr, 16))};
}

double chroma(const Rgb& color) { return std::ranges::max(color) - std::ranges::min(color); }

double luminance(const Rgb& color) {
    auto linear = [](double channel) {
        channel /= 255;
        return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(color[0]) + 0.7152 * linear(color[1]) + 0.0722 * linear(color[2]);
}

// The most common saturated colour among the image's opaque pixels, ignoring near-neutrals.
std::optional<Rgb> dominant_color(const std::filesystem::path& image_path) {
    VImage image = VImage::thumbnail(image_path.c_str(), 64).colourspace(VIPS_INTERPRETATION_sRGB);
    if (image.bands() == 2 || image.bands() == 4) {
        image = image.flatten(VImage::option()->set("background", std::vector<double>{255, 255, 255}));
    }
    if (image.bands() == 1) image = image.bandjoin({image, image});
    image = image.cast(VIPS_FORMAT_UCHAR);
    size_t size = 0;
    auto* pixels = static_cast<unsigned char*>(image.write_to_memory(&size));
    std::map<int, std::pair<int, Rgb>> buckets;
    for (size_t i = 0; i + 2 < size; i += 3) {
        Rgb color{double(pixels[i]), double(pixels[i + 1]), double(pixels[i + 2])};
        if (chroma(color) < 48) continue;
        auto& [count, sum] = buckets[(pixels[i] >> 5) << 6 | (pixels[i + 1] >> 5) << 3 | (pixels[i + 2] >> 5)];
        ++count;
        for (int c = 0; c < 3; ++c) sum[c] += color[c];
    }
    g_free(pixels);
    auto best = std::ranges::max_element(buckets, {}, [](const auto& entry) { return entry.second.first; });
    if (best == buckets.end() || best->second.first < 8) return std::nullopt;
    auto [count, sum] = best->second;
    return Rgb{sum[0] / count, sum[1] / count, sum[2] / count};
}

// Theme colours come first because the site chose them. Pale ones are skipped: the accent carries white text.
Rgb accent_color(const CoverSpec& spec) {
    for (const auto& value : spec.theme_colors) {
        if (auto color = parse_hex_color(value); color && luminance(*color) < 0.35) return *color;
    }
    for (const auto& path : {spec.logo, spec.icon}) {
        try {
            if (auto color = path ? dominant_color(*path) : std::nullopt; color && luminance(*color) < 0.35)
                return *color;
        } catch (const vips::VError&) {
        }
    }
    return {0x24, 0x2b, 0x38};
}

std::vector<double> as_vector(const Rgb& color) { return {color[0], color[1], color[2]}; }

// Pango markup text rendered as a mask, word-wrapped to width.
VImage text_mask(std::string_view text, const std::string& font, int width, int spacing = 0) {
    auto options =
        VImage::option()->set("font", font.c_str())->set("width", width)->set("wrap", VIPS_TEXT_WRAP_WORD_CHAR);
    if (spacing) options->set("spacing", spacing);
    return VImage::text(escape_html(text).c_str(), options);
}

// Largest font size, down to a floor, at which the title fits the box. A title that still overflows at
// the floor is cut at the box edge on a line boundary; titles come from site names, so this is rare.
VImage fit_title(std::string_view title, int width, int max_height) {
    for (int size = 150; size > 64; size -= 8) {
        VImage mask = text_mask(title, "sans bold " + std::to_string(size), width, size / 5);
        if (mask.height() <= max_height) return mask;
    }
    VImage mask = text_mask(title, "sans bold 64", width, 12);
    constexpr int line_height = 64 * 96 / 72 + 12;
    return mask.height() <= max_height ? mask : mask.crop(0, 0, mask.width(), max_height - max_height % line_height);
}

// Mean luminance of the image's visible pixels, weighted by alpha, or nullopt when it is fully transparent.
std::optional<double> ink_luminance(const std::filesystem::path& path) {
    // copy_memory: the statistics below read each band separately, which a sequential PNG load rejects.
    VImage image = VImage::thumbnail(path.c_str(), 128)
                       .colourspace(VIPS_INTERPRETATION_sRGB)
                       .cast(VIPS_FORMAT_UCHAR)
                       .copy_memory();
    if (image.bands() == 1) image = image.bandjoin({image, image});
    VImage alpha = image.bands() >= 4 ? image[3] : VImage::black(image.width(), image.height()) + 255;
    double weight = alpha.avg() * image.width() * image.height();
    if (weight < 1) return std::nullopt;
    auto channel = [&](int band) { return (image[band] * alpha).avg() * image.width() * image.height() / weight; };
    return luminance({channel(0), channel(1), channel(2)});
}

// Reads the image, flattens alpha onto the given background, and scales it to fit the box.
VImage fitted_image(const std::filesystem::path& path, int max_width, int max_height, const Rgb& background) {
    VImage image = VImage::thumbnail(path.c_str(), max_width, VImage::option()->set("height", max_height))
                       .colourspace(VIPS_INTERPRETATION_sRGB);
    if (image.bands() == 2 || image.bands() == 4) {
        image = image.flatten(VImage::option()->set("background", as_vector(background)));
    }
    if (image.bands() == 1) image = image.bandjoin({image, image});
    return image.cast(VIPS_FORMAT_UCHAR);
}

}  // namespace

void render_cover(const CoverSpec& spec, const std::filesystem::path& jpeg) {
    ensure_vips();
    constexpr int margin = 150;
    constexpr int band_height = 300;
    constexpr int text_width = cover_width - 2 * margin;
    const Rgb paper{0xfa, 0xf8, 0xf4};
    const Rgb ink{0x1c, 0x1e, 0x22};
    const Rgb muted{0x5c, 0x5f, 0x66};
    const Rgb accent = accent_color(spec);

    VImage canvas = VImage::black(cover_width, cover_height).new_from_image(as_vector(paper)).cast(VIPS_FORMAT_UCHAR);
    auto paint = [&](const VImage& mask, int left, int top, const Rgb& color) {
        VImage placed = mask.embed(left, top, cover_width, cover_height);
        canvas =
            placed.ifthenelse(as_vector(color), canvas, VImage::option()->set("blend", true)).cast(VIPS_FORMAT_UCHAR);
    };
    auto fill = [&](int left, int top, int width, int height, const Rgb& color) {
        VImage block = VImage::black(width, height).new_from_image(as_vector(color)).cast(VIPS_FORMAT_UCHAR);
        canvas = canvas.insert(block, left, top);
    };

    // A light logo (a dark-theme variant) vanishes on paper, so it moves onto a taller accent band.
    std::optional<std::filesystem::path> mark = spec.logo ? spec.logo : spec.icon;
    bool light_mark = false;
    try {
        auto ink = mark ? ink_luminance(*mark) : std::nullopt;
        light_mark = ink && *ink > 0.6 && luminance(accent) < 0.2;
    } catch (const vips::VError&) {
        mark.reset();
    }
    int band = light_mark ? band_height + 420 : band_height;
    fill(0, 0, cover_width, band, accent);
    paint(text_mask("DOCUMENTATION", "sans bold 34", text_width), margin, 190 - 34, {0xff, 0xff, 0xff});

    // The logo, title, rule, and description form one block. It is measured first, then placed a third of
    // the way down the space between the band and the footer, so short titles do not leave the bottom empty.
    struct Layer {
        VImage image;
        std::optional<Rgb> tint;  // set for text masks; unset for pixels inserted as-is
        int gap_after;
    };
    std::vector<Layer> block;
    if (mark) {
        try {
            VImage image =
                fitted_image(*mark, spec.logo ? 900 : 280, spec.logo ? 220 : 280, light_mark ? accent : paper);
            if (light_mark) {
                canvas = canvas.insert(image, margin, 300);
            } else {
                block.push_back({image, std::nullopt, 150});
            }
        } catch (const vips::VError&) {
        }
    }
    auto title = strip_whitespace(spec.title);
    if (!title.empty()) block.push_back({fit_title(title, text_width, 760), ink, 70});
    block.push_back({VImage::black(180, 14) + 255, accent, 100});

    constexpr int footer_top = cover_height - 330;
    constexpr int minimum_gap = 150;
    int area_top = band + minimum_gap;
    int area_bottom = footer_top - minimum_gap;
    int fixed = 0;
    for (const auto& layer : block) fixed += layer.image.height() + layer.gap_after;

    // A description too long for the space left loses whole words from the end, then gains an ellipsis.
    auto description = strip_whitespace(spec.description);
    while (!description.empty()) {
        VImage mask = text_mask(description, "serif 46", text_width, 18);
        if (mask.height() <= area_bottom - area_top - fixed) {
            block.push_back({mask, muted, 0});
            break;
        }
        auto cut = description.find_last_of(' ', description.size() - 2);
        description = cut == std::string::npos ? "" : strip_whitespace(description.substr(0, cut)) + "\u2026";
    }

    int total = 0;
    for (const auto& layer : block) total += layer.image.height() + layer.gap_after;
    int top = area_top + std::max(0, (area_bottom - area_top - total) / 3);
    for (const auto& layer : block) {
        if (layer.tint) {
            paint(layer.image, margin, top, *layer.tint);
        } else {
            canvas = canvas.insert(layer.image, margin, top);
        }
        top += layer.image.height() + layer.gap_after;
    }

    fill(margin, footer_top, text_width, 3, {0xd8, 0xd4, 0xcc});
    if (!spec.host.empty()) paint(text_mask(spec.host, "sans bold 44", text_width), margin, cover_height - 280, ink);
    if (!spec.captured_on.empty()) {
        paint(text_mask("Captured " + spec.captured_on, "sans 34", text_width), margin, cover_height - 200, muted);
    }
    canvas.jpegsave(jpeg.c_str(), VImage::option()->set("Q", 90)->set("optimize_coding", true));
}

}  // namespace webspine
