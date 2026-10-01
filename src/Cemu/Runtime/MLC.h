#pragma once

#include <filesystem>

namespace Cemu::Runtime
{
// Create the stock skeleton without replacing existing language/country files.
[[nodiscard]] bool CreateDefaultMLCFiles(const std::filesystem::path& mlc) noexcept;
}
