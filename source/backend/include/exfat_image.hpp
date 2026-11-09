#pragma once

#include <cstddef>
#include <string>

namespace prospero {

// Reads one small file (e.g. "sce_sys/param.json") out of an exFAT disk image without mounting it.
bool ReadExfatFile(const std::string &image_path, const std::string &inner_path, size_t max_bytes, std::string *out);

} // namespace prospero
