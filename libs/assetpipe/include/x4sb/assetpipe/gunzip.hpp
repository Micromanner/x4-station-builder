#pragma once

#include <optional>
#include <string>
#include <vector>

namespace x4sb {

// Minimal gunzip wrapper over sinfl for decompressing X4 texture .gz files.
// Returns nullopt if the gzip header is invalid or decompression fails.
std::optional<std::vector<char>> gunzip(const std::string& gzipped);

}  // namespace x4sb
