#pragma once

#include "model.hpp"

#include <filesystem>
#include <optional>

namespace docs2epub {

StageResult validate(const std::filesystem::path& epub, const std::optional<std::filesystem::path>& workspace,
                     bool reflow);

}  // namespace docs2epub
