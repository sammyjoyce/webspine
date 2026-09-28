cmake_minimum_required(VERSION 3.24)
project(webspine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET libxml-2.0 libzip libcurl vips-cpp)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(CLI11 REQUIRED)

file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/book.css BOOK_CSS)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/book_css.hpp CONTENT
  "#pragma once\n#include <string_view>\nnamespace webspine {\ninline constexpr std::string_view book_css = R\"css(${BOOK_CSS})css\";\n}\n")

add_library(webspine_lib STATIC
  src/browser.cpp
  src/build.cpp
  src/core.cpp
  src/html.cpp
  src/media.cpp
  src/model.cpp
  src/scrape.cpp
  src/validate.cpp
  src/workspace.cpp
)
target_include_directories(webspine_lib PUBLIC src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_options(webspine_lib PRIVATE -Wall -Wextra)
target_link_libraries(webspine_lib PUBLIC PkgConfig::DEPS OpenSSL::Crypto nlohmann_json::nlohmann_json)

add_executable(webspine src/main.cpp)
target_link_libraries(webspine PRIVATE webspine_lib CLI11::CLI11)
install(TARGETS webspine)

option(WEBSPINE_BUILD_TESTS "Build the test suite" ON)
if(WEBSPINE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
