#pragma once

#include <string>

namespace xiaozhi {

std::string GetDeviceId();
std::string LoadOrCreateClientId(const char *path,
                                 const std::string &fallback_seed);

} // namespace xiaozhi
