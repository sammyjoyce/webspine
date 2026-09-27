#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace docs2epub {

inline constexpr std::string_view version = "0.1.0";

struct Url {
    std::string scheme;
    std::string netloc;
    std::string path;
    std::string query;
    std::string fragment;
};

Url parse_url(std::string_view url);
std::string unparse_url(const Url& url);
std::string url_join(std::string_view base, std::string_view reference);
std::pair<std::string, std::string> url_defrag(std::string_view url);
std::string canonical_url(std::string_view url);
bool in_scope(std::string_view url, std::string_view base_url);

std::string sha256_hex(std::string_view data);
std::string uuid5_url(std::string_view name);

std::string route_name(std::string_view route);
std::string xml_id(std::string_view value);
std::string page_key(std::string_view url);
std::string chapter_name(std::string_view route, std::string_view url);
std::filesystem::path default_workspace(std::string_view url);

std::string escape_html(std::string_view text);
std::string strip_whitespace(std::string_view text);
bool starts_with_any(std::string_view text, std::initializer_list<std::string_view> prefixes);

}  // namespace docs2epub
