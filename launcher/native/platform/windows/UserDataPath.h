#pragma once

#include <filesystem>

namespace mcwiiu
{
// Resolve private runtime storage independently of the working directory.
[[nodiscard]] std::filesystem::path WebViewStoragePath();
}
