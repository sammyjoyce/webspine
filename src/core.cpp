#include "core.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace webspine {
namespace {

std::string strip_chars(std::string_view text, std::string_view chars) {
    auto first = text.find_first_not_of(chars);
    if (first == std::string_view::npos) return {};
    auto last = text.find_last_not_of(chars);
    return std::string(text.substr(first, last - first + 1));
}

template <typename Keep>
std::string replace_runs(std::string_view text, Keep keep) {
    std::string out;
    bool in_run = false;
    for (char c : text) {
        if (keep(static_cast<unsigned char>(c))) {
            out += c;
            in_run = false;
        } else if (!in_run) {
            out += '-';
            in_run = true;
        }
    }
    return out;
}

bool uses_netloc(std::string_view scheme) {
    return scheme.empty() || scheme == "http" || scheme == "https" || scheme == "file" || scheme == "ftp" ||
           scheme == "ws" || scheme == "wss";
}

std::string remove_dot_segments(std::string_view path) {
    std::vector<std::string> output;
    std::string input(path);
    bool absolute = !input.empty() && input.front() == '/';
    std::vector<std::string> segments;
    size_t start = absolute ? 1 : 0;
    while (true) {
        auto slash = input.find('/', start);
        segments.push_back(input.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    for (size_t i = 0; i < segments.size(); ++i) {
        const auto& segment = segments[i];
        bool last = i + 1 == segments.size();
        if (segment == ".") {
            if (last) output.emplace_back();
        } else if (segment == "..") {
            if (!output.empty()) output.pop_back();
            if (last) output.emplace_back();
        } else {
            output.push_back(segment);
        }
    }
    std::string result = absolute ? "/" : "";
    for (size_t i = 0; i < output.size(); ++i) {
        if (i) result += '/';
        result += output[i];
    }
    return result;
}

std::array<unsigned char, EVP_MAX_MD_SIZE> digest(const EVP_MD* type, std::string_view data, unsigned int& size) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> out{};
    if (!EVP_Digest(data.data(), data.size(), out.data(), &size, type, nullptr)) {
        throw std::runtime_error("OpenSSL digest failed");
    }
    return out;
}

std::string hex(const unsigned char* bytes, size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < size; ++i) {
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 15];
    }
    return out;
}

}  // namespace

std::string strip_whitespace(std::string_view text) { return strip_chars(text, " \t\n\v\f\r"); }

bool starts_with_any(std::string_view text, std::initializer_list<std::string_view> prefixes) {
    return std::any_of(prefixes.begin(), prefixes.end(),
                       [&](std::string_view prefix) { return text.starts_with(prefix); });
}

Url parse_url(std::string_view url) {
    Url out;
    std::string_view rest = url;
    auto colon = rest.find(':');
    if (colon != std::string_view::npos && colon > 0 && std::isalpha(static_cast<unsigned char>(rest[0]))) {
        auto scheme = rest.substr(0, colon);
        bool valid = std::all_of(scheme.begin(), scheme.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.';
        });
        if (valid) {
            for (char c : scheme) out.scheme += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            rest.remove_prefix(colon + 1);
        }
    }
    if (rest.starts_with("//")) {
        rest.remove_prefix(2);
        auto end = rest.find_first_of("/?#");
        out.netloc = std::string(rest.substr(0, end));
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end);
    }
    if (auto hash = rest.find('#'); hash != std::string_view::npos) {
        out.fragment = std::string(rest.substr(hash + 1));
        rest = rest.substr(0, hash);
    }
    if (auto question = rest.find('?'); question != std::string_view::npos) {
        out.query = std::string(rest.substr(question + 1));
        rest = rest.substr(0, question);
    }
    out.path = std::string(rest);
    return out;
}

std::string unparse_url(const Url& url) {
    std::string body = url.path;
    if (!url.netloc.empty() || (!url.scheme.empty() && uses_netloc(url.scheme) && !body.starts_with("//"))) {
        if (!body.empty() && body.front() != '/') body.insert(body.begin(), '/');
        body = "//" + url.netloc + body;
    }
    std::string out = url.scheme.empty() ? body : url.scheme + ":" + body;
    if (!url.query.empty()) out += "?" + url.query;
    if (!url.fragment.empty()) out += "#" + url.fragment;
    return out;
}

std::string url_join(std::string_view base, std::string_view reference) {
    if (base.empty()) return std::string(reference);
    if (reference.empty()) return std::string(base);
    Url b = parse_url(base);
    Url r = parse_url(reference);
    if (!r.scheme.empty() && r.scheme != b.scheme) return std::string(reference);
    Url t;
    t.scheme = b.scheme;
    t.fragment = r.fragment;
    std::string_view after_scheme = reference;
    if (!r.scheme.empty()) after_scheme.remove_prefix(r.scheme.size() + 1);
    if (after_scheme.starts_with("//")) {
        t.netloc = r.netloc;
        t.path = remove_dot_segments(r.path);
        t.query = r.query;
        return unparse_url(t);
    }
    t.netloc = b.netloc;
    if (r.path.empty()) {
        t.path = b.path;
        t.query = r.query.empty() ? b.query : r.query;
    } else {
        t.query = r.query;
        if (r.path.front() == '/') {
            t.path = remove_dot_segments(r.path);
        } else if (!b.netloc.empty() && b.path.empty()) {
            t.path = remove_dot_segments("/" + r.path);
        } else {
            auto slash = b.path.rfind('/');
            std::string merged = slash == std::string::npos ? r.path : b.path.substr(0, slash + 1) + r.path;
            t.path = remove_dot_segments(merged);
        }
    }
    return unparse_url(t);
}

std::pair<std::string, std::string> url_defrag(std::string_view url) {
    auto hash = url.find('#');
    if (hash == std::string_view::npos) return {std::string(url), ""};
    return {std::string(url.substr(0, hash)), std::string(url.substr(hash + 1))};
}

std::string canonical_url(std::string_view url) {
    Url parsed = parse_url(url_defrag(url).first);
    std::string path;
    for (char c : parsed.path) {
        if (c == '/' && !path.empty() && path.back() == '/') continue;
        path += c;
    }
    if (path.empty()) path = "/";
    if (path != "/") {
        while (path.size() > 1 && path.back() == '/') path.pop_back();
    }
    return unparse_url({parsed.scheme, parsed.netloc, path, "", ""});
}

bool in_scope(std::string_view url, std::string_view base_url) {
    Url target = parse_url(url);
    Url base = parse_url(base_url);
    std::string base_path = base.path;
    while (!base_path.empty() && base_path.back() == '/') base_path.pop_back();
    return (target.scheme == "http" || target.scheme == "https") && target.netloc == base.netloc &&
           (base_path.empty() || target.path == base_path || target.path.starts_with(base_path + "/"));
}

std::string sha256_hex(std::string_view data) {
    unsigned int size = 0;
    auto bytes = digest(EVP_sha256(), data, size);
    return hex(bytes.data(), size);
}

std::string uuid5_url(std::string_view name) {
    static constexpr unsigned char namespace_url[16] = {0x6b, 0xa7, 0xb8, 0x11, 0x9d, 0xad, 0x11, 0xd1,
                                                        0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8};
    std::string data(reinterpret_cast<const char*>(namespace_url), 16);
    data += name;
    unsigned int size = 0;
    auto bytes = digest(EVP_sha1(), data, size);
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x50);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
    std::string digits = hex(bytes.data(), 16);
    return digits.substr(0, 8) + "-" + digits.substr(8, 4) + "-" + digits.substr(12, 4) + "-" + digits.substr(16, 4) +
           "-" + digits.substr(20, 12);
}

std::string route_name(std::string_view route) {
    std::string value = strip_chars(route, "/");
    if (value.empty()) value = "index";
    value = replace_runs(value, [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '_' || c == '-'; });
    value = strip_chars(value, "-");
    if (value.empty() || value == "." || value == "..") return "index";
    return value;
}

std::string xml_id(std::string_view value) {
    std::string out = replace_runs(strip_whitespace(value),
                                   [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '.' || c == '-'; });
    out = strip_chars(out, "-");
    if (out.empty()) return "id";
    if (!(std::isalpha(static_cast<unsigned char>(out[0])) || out[0] == '_')) out = "id-" + out;
    return out;
}

std::string page_key(std::string_view url) { return sha256_hex(url).substr(0, 16); }

std::string chapter_name(std::string_view route, std::string_view url) {
    return route_name(route) + "-" + sha256_hex(url).substr(0, 8) + ".xhtml";
}

std::filesystem::path default_workspace(std::string_view url) {
    Url parsed = parse_url(url);
    std::string path = strip_chars(parsed.path, "/");
    std::replace(path.begin(), path.end(), '/', '-');
    if (path.empty()) path = "root";
    return std::filesystem::path(".webspine") / (parsed.netloc + "-" + path);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string base64_encode(std::string_view data) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < data.size(); i += 3) {
        size_t left = std::min<size_t>(3, data.size() - i);
        unsigned n = static_cast<unsigned char>(data[i]) << 16;
        if (left > 1) n |= static_cast<unsigned char>(data[i + 1]) << 8;
        if (left > 2) n |= static_cast<unsigned char>(data[i + 2]);
        out += table[n >> 18];
        out += table[(n >> 12) & 63];
        out += left > 1 ? table[(n >> 6) & 63] : '=';
        out += left > 2 ? table[n & 63] : '=';
    }
    return out;
}

std::string base64_decode(std::string_view data) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    unsigned buffer = 0;
    int bits = 0;
    for (char c : data) {
        int v = value(c);
        if (v < 0) continue;
        buffer = (buffer << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buffer >> bits) & 0xff);
        }
    }
    return out;
}

bool valid_utf8(std::string_view data) {
    for (size_t i = 0; i < data.size();) {
        auto c = static_cast<unsigned char>(data[i]);
        size_t length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xe ? 3 : (c >> 3) == 0x1e ? 4 : 0;
        if (length == 0 || i + length > data.size()) return false;
        uint32_t code = length == 1 ? c : c & (0x7f >> length);
        for (size_t k = 1; k < length; ++k) {
            auto next = static_cast<unsigned char>(data[i + k]);
            if ((next >> 6) != 0x2) return false;
            code = (code << 6) | (next & 0x3f);
        }
        static constexpr uint32_t minimum[] = {0, 0, 0x80, 0x800, 0x10000};
        if (code < minimum[length] || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
        i += length;
    }
    return true;
}

bool declares_charset(std::string_view html) {
    auto head = lower(std::string(html.substr(0, 1024)));
    for (size_t at = head.find("<meta"); at != std::string::npos; at = head.find("<meta", at + 5)) {
        auto tag = head.substr(at, head.find('>', at) - at);
        if (tag.find("charset") != std::string::npos) return true;
    }
    return false;
}

std::string escape_html(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&#x27;";
                break;
            default:
                out += c;
        }
    }
    return out;
}

}  // namespace webspine
